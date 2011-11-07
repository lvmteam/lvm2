/*
 * Copyright (C) 2009 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "clvmd-common.h"

#include <pthread.h>

#include "locking.h"
#include "clvm.h"
#include "clvmd-comms.h"
#include "lvm-functions.h"
#include "clvmd.h"

#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>

static const char SINGLENODE_CLVMD_SOCKNAME[] = DEFAULT_RUN_DIR "/clvmd_singlenode.sock";
static int listen_fd = -1;
static int *_locks = NULL;
static char **_resources = NULL;

static void close_comms(void)
{
	if (listen_fd != -1 && close(listen_fd))
		stack;
	(void)unlink(SINGLENODE_CLVMD_SOCKNAME);
	listen_fd = -1;
}

static int init_comms(void)
{
	struct sockaddr_un addr;
	mode_t old_mask;

	close_comms();

	(void) dm_prepare_selinux_context(SINGLENODE_CLVMD_SOCKNAME, S_IFSOCK);
	old_mask = umask(0077);

	listen_fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		DEBUGLOG("Can't create local socket: %s\n", strerror(errno));
		goto error;
	}
	/* Set Close-on-exec */
	if (fcntl(listen_fd, F_SETFD, 1)) {
		DEBUGLOG("Setting CLOEXEC on client fd failed: %s\n", strerror(errno));
		goto error;
	}

	memset(&addr, 0, sizeof(addr));
	memcpy(addr.sun_path, SINGLENODE_CLVMD_SOCKNAME,
	       sizeof(SINGLENODE_CLVMD_SOCKNAME));
	addr.sun_family = AF_UNIX;

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		DEBUGLOG("Can't bind local socket: %s\n", strerror(errno));
		goto error;
	}
	if (listen(listen_fd, 10) < 0) {
		DEBUGLOG("Can't listen local socket: %s\n", strerror(errno));
		goto error;
	}

	umask(old_mask);
	(void) dm_prepare_selinux_context(NULL, 0);
	return 0;
error:
	umask(old_mask);
	(void) dm_prepare_selinux_context(NULL, 0);
	close_comms();
	return -1;
}

static int _init_cluster(void)
{
	int r;

	r = init_comms();
	if (r)
		return r;

	DEBUGLOG("Single-node cluster initialised.\n");
	return 0;
}

static void _cluster_closedown(void)
{
	close_comms();

	DEBUGLOG("cluster_closedown\n");
	destroy_lvhash();
	dm_free(_locks);
	dm_free(_resources);
	_locks = NULL;
	_resources = NULL;
}

static void _get_our_csid(char *csid)
{
	int nodeid = 1;
	memcpy(csid, &nodeid, sizeof(int));
}

static int _csid_from_name(char *csid, const char *name)
{
	return 1;
}

static int _name_from_csid(const char *csid, char *name)
{
	sprintf(name, "SINGLENODE");
	return 0;
}

static int _get_num_nodes(void)
{
	return 1;
}

/* Node is now known to be running a clvmd */
static void _add_up_node(const char *csid)
{
}

/* Call a callback for each node, so the caller knows whether it's up or down */
static int _cluster_do_node_callback(struct local_client *master_client,
				     void (*callback)(struct local_client *,
				     const char *csid, int node_up))
{
	return 0;
}

int _lock_file(const char *file, uint32_t flags);

static int _lock_max = 1;
static pthread_mutex_t _lock_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Using one common condition for all locks for simplicity */
static pthread_cond_t _lock_cond = PTHREAD_COND_INITIALIZER;

/* Real locking */
static int _lock_resource(const char *resource, int mode, int flags, int *lockid)
{
	int *_locks_1;
	char **_resources_1;
	int i, j;

	if (mode == LCK_READ) { /* only track PREAD, aka PROTECTED READ */
		DEBUGLOG("Not tracking CONCURRENT READ lock: %s, flags=%d, mode=%d\n",
			 resource, flags, mode);
		*lockid = -1;
		return 0;
	}

	DEBUGLOG("Locking resource %s, flags=%d, mode=%d\n",
		 resource, flags, mode);

	pthread_mutex_lock(&_lock_mutex);
retry:

	/* look for an existing lock for this resource */
	for (i = 1; i < _lock_max; ++i) {
		if (!_resources[i])
			break;
		if (!strcmp(_resources[i], resource)) {
			if ((_locks[i] & LCK_TYPE_MASK) == LCK_WRITE ||
			    (_locks[i] & LCK_TYPE_MASK) == LCK_EXCL) {
				DEBUGLOG("Resource %s already write/exclusively locked...\n", resource);
				goto maybe_retry;
			}
			if ((mode & LCK_TYPE_MASK) == LCK_WRITE ||
			    (mode & LCK_TYPE_MASK) == LCK_EXCL) {
				DEBUGLOG("Resource %s already locked and WRITE/EXCL lock requested...\n",
					 resource);
				goto maybe_retry;
			}
		}
	}

	if (i == _lock_max) { /* out of lock slots, extend */
		if (!(_locks_1 = dm_realloc(_locks, 2 * _lock_max * sizeof(int))))
			goto_bad;

		_locks = _locks_1;
		if (!(_resources_1 = dm_realloc(_resources, 2 * _lock_max * sizeof(char *))))
			/* _locks may get realloc'd twice, but that should be safe */
			goto_bad;

		_resources = _resources_1;
		/* clear the new resource entries */
		for (j = _lock_max; j < 2 * _lock_max; ++j)
			_resources[j] = NULL;
		_lock_max = 2 * _lock_max;
	}

	/* resource is not currently locked, grab it */
	if (!(_resources[i] = dm_strdup(resource)))
		goto_bad;

	*lockid = i;
	_locks[i] = mode;

	DEBUGLOG("Locked resource %s, lockid=%d\n", resource, i);
	pthread_mutex_unlock(&_lock_mutex);

	return 0;

maybe_retry:
	if (!(flags & LCK_NONBLOCK)) {
		pthread_cond_wait(&_lock_cond, &_lock_mutex);
		goto retry;
	}
bad:
	DEBUGLOG("Failed to lock resource %s\n", resource);
	pthread_mutex_unlock(&_lock_mutex);

	return 1; /* fail */
}

static int _unlock_resource(const char *resource, int lockid)
{
	if (lockid < 0) {
		DEBUGLOG("Not tracking unlock of lockid -1: %s, lockid=%d\n",
			 resource, lockid);
		return 0;
	}

	DEBUGLOG("Unlocking resource %s, lockid=%d\n", resource, lockid);
	pthread_mutex_lock(&_lock_mutex);

	if (!_resources[lockid]) {
		pthread_mutex_unlock(&_lock_mutex);
		DEBUGLOG("Resource %s, lockid=%d is not locked\n", resource, lockid);
		return 1;
	}

	if (strcmp(_resources[lockid], resource)) {
		pthread_mutex_unlock(&_lock_mutex);
		DEBUGLOG("Resource %d has wrong resource (requested %s, got %s)\n",
			 lockid, resource, _resources[lockid]);
		return 1;
	}

	dm_free(_resources[lockid]);
	_resources[lockid] = 0;
	pthread_cond_broadcast(&_lock_cond); /* wakeup waiters */
	pthread_mutex_unlock(&_lock_mutex);

	return 0;
}

static int _is_quorate(void)
{
	return 1;
}

static int _get_main_cluster_fd(void)
{
	return listen_fd;
}

static int _cluster_fd_callback(struct local_client *fd, char *buf, int len,
				const char *csid,
				struct local_client **new_client)
{
	return 1;
}

static int _cluster_send_message(const void *buf, int msglen,
				 const char *csid,
				 const char *errtext)
{
	return 0;
}

static int _get_cluster_name(char *buf, int buflen)
{
	strncpy(buf, "localcluster", buflen);
	buf[buflen - 1] = 0;
	return 0;
}

static struct cluster_ops _cluster_singlenode_ops = {
	.name                     = "singlenode",
	.cluster_init_completed   = NULL,
	.cluster_send_message     = _cluster_send_message,
	.name_from_csid           = _name_from_csid,
	.csid_from_name           = _csid_from_name,
	.get_num_nodes            = _get_num_nodes,
	.cluster_fd_callback      = _cluster_fd_callback,
	.get_main_cluster_fd      = _get_main_cluster_fd,
	.cluster_do_node_callback = _cluster_do_node_callback,
	.is_quorate               = _is_quorate,
	.get_our_csid             = _get_our_csid,
	.add_up_node              = _add_up_node,
	.reread_config            = NULL,
	.cluster_closedown        = _cluster_closedown,
	.get_cluster_name         = _get_cluster_name,
	.sync_lock                = _lock_resource,
	.sync_unlock              = _unlock_resource,
};

struct cluster_ops *init_singlenode_cluster(void)
{
	if (!_init_cluster())
		return &_cluster_singlenode_ops;
	else
		return NULL;
}
