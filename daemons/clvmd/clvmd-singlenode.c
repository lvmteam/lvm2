/*
 * Copyright (C) 2009-2013 Red Hat, Inc. All rights reserved.
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

static struct dm_hash_table *_locks;
static int _lockid;

static pthread_mutex_t _lock_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Using one common condition for all locks for simplicity */
static pthread_cond_t _lock_cond = PTHREAD_COND_INITIALIZER;

struct lock {
	struct dm_list list;
	int lockid;
	int mode;
};

static void close_comms(void)
{
	if (listen_fd != -1 && close(listen_fd))
		stack;
	(void)unlink(SINGLENODE_CLVMD_SOCKNAME);
	listen_fd = -1;
}

static int init_comms(void)
{
	mode_t old_mask;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };

	if (!dm_strncpy(addr.sun_path, SINGLENODE_CLVMD_SOCKNAME,
			sizeof(addr.sun_path))) {
		DEBUGLOG("%s: singlenode socket name too long.",
			 SINGLENODE_CLVMD_SOCKNAME);
		return -1;
	}

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

	if (!(_locks = dm_hash_create(128))) {
		DEBUGLOG("Failed to allocate single-node hash table.\n");
		return 1;
	}

	r = init_comms();
	if (r) {
		dm_hash_destroy(_locks);
		return r;
	}

	DEBUGLOG("Single-node cluster initialised.\n");
	return 0;
}

static void _cluster_closedown(void)
{
	close_comms();

	DEBUGLOG("cluster_closedown\n");
	destroy_lvhash();
	/* If there is any awaited resource, kill it softly */
	pthread_mutex_lock(&_lock_mutex);
	dm_hash_destroy(_locks);
	_locks = NULL;
	_lockid = 0;
	pthread_cond_broadcast(&_lock_cond); /* wakeup waiters */
	pthread_mutex_unlock(&_lock_mutex);
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
	strcpy(name, "SINGLENODE");
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

static const char *_get_mode(int mode)
{
	switch (mode) {
	case LCK_NULL: return "NULL";
	case LCK_READ: return "READ";
	case LCK_PREAD: return "PREAD";
	case LCK_WRITE: return "WRITE";
	case LCK_EXCL: return "EXCLUSIVE";
	case LCK_UNLOCK: return "UNLOCK";
	default: return "????";
	}
}

/* Real locking */
static int _lock_resource(const char *resource, int mode, int flags, int *lockid)
{
	/* DLM table of allowed transition states */
	static const int _dlm_table[6][6] = {
	/* Mode	   NL	CR	CW	PR	PW	EX */
	/* NL */ { 1,	 1,	 1,	 1,	 1,	 1},
	/* CR */ { 1,	 1,	 1,	 1,	 1,	 0},
	/* CW */ { 1,	 1,	 1,	 0,	 0,	 0},
	/* PR */ { 1,	 1,	 0,	 1,	 0,	 0},
	/* PW */ { 1,	 1,	 0,	 0,	 0,	 0},
	/* EX */ { 1,	 0,	 0,	 0,	 0,	 0}
	};

	struct lock *lck = NULL, *lckt;
	struct dm_list *head;

	DEBUGLOG("Locking resource %s, flags=0x%02x (%s%s%s), mode=%s (%d)\n",
		 resource, flags,
		 (flags & LCKF_NOQUEUE) ? "NOQUEUE" : "",
		 ((flags & (LCKF_NOQUEUE | LCKF_CONVERT)) ==
		  (LCKF_NOQUEUE | LCKF_CONVERT)) ? "|" : "",
		 (flags & LCKF_CONVERT) ? "CONVERT" : "",
		 _get_mode(mode), mode);

	mode &= LCK_TYPE_MASK;
	pthread_mutex_lock(&_lock_mutex);

retry:
	pthread_cond_broadcast(&_lock_cond); /* to wakeup waiters */

	if (!(head = dm_hash_lookup(_locks, resource))) {
		if (flags & LCKF_CONVERT) {
			/* In real DLM, lock is identified only by lockid, resource is not used */
			DEBUGLOG("Unlocked resource %s cannot be converted\n", resource);
			goto_bad;
		}
		/* Add new locked resource */
		if (!(head = dm_malloc(sizeof(struct dm_list))) ||
		    !dm_hash_insert(_locks, resource, head)) {
			dm_free(head);
			goto_bad;
		}

		dm_list_init(head);
	} else	/* Update/convert locked resource */
		dm_list_iterate_items(lck, head) {
			/* Check is all locks are compatible with requested lock */
			if (flags & LCKF_CONVERT) {
				if (lck->lockid != *lockid)
					continue;

				DEBUGLOG("Converting resource %s lockid=%d mode:%s -> %s...\n",
					 resource, lck->lockid, _get_mode(lck->mode), _get_mode(mode));
				dm_list_iterate_items(lckt, head) {
					if ((lckt->lockid != *lockid) &&
					    !_dlm_table[mode][lckt->mode]) {
						if (!(flags & LCKF_NOQUEUE) &&
						    /* TODO: Real dlm uses here conversion queues */
						    !pthread_cond_wait(&_lock_cond, &_lock_mutex) &&
						    _locks) /* End of the game? */
							goto retry;
						goto bad;
					}
				}
				lck->mode = mode; /* Lock is now converted */
				goto out;
			} else if (!_dlm_table[mode][lck->mode]) {
				DEBUGLOG("Resource %s already locked lockid=%d, mode:%s\n",
					 resource, lck->lockid, _get_mode(lck->mode));
				if (!(flags & LCKF_NOQUEUE) &&
				    !pthread_cond_wait(&_lock_cond, &_lock_mutex) &&
				    _locks) { /* End of the game? */
					DEBUGLOG("Resource %s retrying lock in mode:%s...\n",
						 resource, _get_mode(mode));
					goto retry;
				}
				goto bad;
			}
		}

	if (!(flags & LCKF_CONVERT)) {
		if (!(lck = dm_malloc(sizeof(struct lock))))
			goto_bad;

		*lockid = lck->lockid = ++_lockid;
		lck->mode = mode;
		dm_list_add(head, &lck->list);
	}
out:
	pthread_mutex_unlock(&_lock_mutex);
	DEBUGLOG("Locked resource %s, lockid=%d, mode=%s\n",
		 resource, lck->lockid, _get_mode(lck->mode));

	return 0;
bad:
	pthread_mutex_unlock(&_lock_mutex);
	DEBUGLOG("Failed to lock resource %s\n", resource);

	return 1; /* fail */
}

static int _unlock_resource(const char *resource, int lockid)
{
	struct lock *lck;
	struct dm_list *head;
	int r = 1;

	if (lockid < 0) {
		DEBUGLOG("Not tracking unlock of lockid -1: %s, lockid=%d\n",
			 resource, lockid);
		return 1;
	}

	DEBUGLOG("Unlocking resource %s, lockid=%d\n", resource, lockid);
	pthread_mutex_lock(&_lock_mutex);
	pthread_cond_broadcast(&_lock_cond); /* wakeup waiters */

	if (!(head = dm_hash_lookup(_locks, resource))) {
		pthread_mutex_unlock(&_lock_mutex);
		DEBUGLOG("Resource %s is not locked.\n", resource);
		return 1;
	}

	dm_list_iterate_items(lck, head)
		if (lck->lockid == lockid) {
			dm_list_del(&lck->list);
			dm_free(lck);
			r = 0;
			goto out;
		}

	DEBUGLOG("Resource %s has wrong lockid %d.\n", resource, lockid);
out:
	if (dm_list_empty(head)) {
		//DEBUGLOG("Resource %s is no longer hashed (lockid=%d).\n", resource, lockid);
		dm_hash_remove(_locks, resource);
		dm_free(head);
	}

	pthread_mutex_unlock(&_lock_mutex);

	return r;
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
	return dm_strncpy(buf, "localcluster", buflen) ? 0 : 1;
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

	return NULL;
}
