/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
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

/*
 * Locking functions for LVM.
 * The main purpose of this part of the library is to serialise LVM
 * management operations across a cluster.
 */

#include "lib.h"
#include "clvm.h"
#include "lvm-string.h"
#include "locking.h"
#include "locking_types.h"
#include "toolcontext.h"

#include <assert.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef CLUSTER_LOCKING_INTERNAL
int lock_resource(struct cmd_context *cmd, const char *resource, uint32_t flags, struct logical_volume *lv __attribute__((unused)));
int query_resource(const char *resource, int *mode);
void locking_end(void);
int locking_init(int type, struct dm_config_tree *cf, uint32_t *flags);
#endif

typedef struct lvm_response {
	char node[255];
	char *response;
	int status;
	int len;
} lvm_response_t;

/*
 * This gets stuck at the start of memory we allocate so we
 * can sanity-check it at deallocation time
 */
#define LVM_SIGNATURE 0x434C564D

/*
 * NOTE: the LVMD uses the socket FD as the client ID, this means
 * that any client that calls fork() will inherit the context of
 * it's parent.
 */
static int _clvmd_sock = -1;

/* FIXME Install SIGPIPE handler? */

/* Open connection to the Cluster Manager daemon */
static int _open_local_sock(int suppress_messages)
{
	int local_socket;
	struct sockaddr_un sockaddr = { .sun_family = AF_UNIX };

	if (!dm_strncpy(sockaddr.sun_path, CLVMD_SOCKNAME, sizeof(sockaddr.sun_path))) {
		log_error("%s: clvmd socket name too long.", CLVMD_SOCKNAME);
		return -1;
	}

	/* Open local socket */
	if ((local_socket = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
		log_error_suppress(suppress_messages, "Local socket "
				   "creation failed: %s", strerror(errno));
		return -1;
	}


	if (connect(local_socket,(struct sockaddr *) &sockaddr,
		    sizeof(sockaddr))) {
		int saved_errno = errno;

		log_error_suppress(suppress_messages, "connect() failed "
				   "on local socket: %s", strerror(errno));
		if (close(local_socket))
			stack;

		errno = saved_errno;
		return -1;
	}

	return local_socket;
}

/* Send a request and return the status */
static int _send_request(char *inbuf, int inlen, char **retbuf)
{
	char outbuf[PIPE_BUF] __attribute__((aligned(8)));
	struct clvm_header *outheader = (struct clvm_header *) outbuf;
	int len;
	unsigned off;
	int buflen;
	int err;

	/* Send it to CLVMD */
 rewrite:
	if ( (err = write(_clvmd_sock, inbuf, inlen)) != inlen) {
		if (err == -1 && errno == EINTR)
			goto rewrite;
		log_error("Error writing data to clvmd: %s", strerror(errno));
		return 0;
	}

	/* Get the response */
 reread:
	if ((len = read(_clvmd_sock, outbuf, sizeof(struct clvm_header))) < 0) {
		if (errno == EINTR)
			goto reread;
		log_error("Error reading data from clvmd: %s", strerror(errno));
		return 0;
	}

	if (len == 0) {
		log_error("EOF reading CLVMD");
		errno = ENOTCONN;
		return 0;
	}

	/* Allocate buffer */
	buflen = len + outheader->arglen;
	*retbuf = dm_malloc(buflen);
	if (!*retbuf) {
		errno = ENOMEM;
		return 0;
	}

	/* Copy the header */
	memcpy(*retbuf, outbuf, len);
	outheader = (struct clvm_header *) *retbuf;

	/* Read the returned values */
	off = 1;		/* we've already read the first byte */
	while (off <= outheader->arglen && len > 0) {
		len = read(_clvmd_sock, outheader->args + off,
			   buflen - off - offsetof(struct clvm_header, args));
		if (len > 0)
			off += len;
	}

	/* Was it an error ? */
	if (outheader->status != 0) {
		errno = outheader->status;

		/* Only return an error here if there are no node-specific
		   errors present in the message that might have more detail */
		if (!(outheader->flags & CLVMD_FLAG_NODEERRS)) {
			log_error("cluster request failed: %s", strerror(errno));
			return 0;
		}

	}

	return 1;
}

/* Build the structure header and parse-out wildcard node names */
/* FIXME: Cleanup implicit casts of clvmd_cmd (int, char, uint8_t, etc). */
static void _build_header(struct clvm_header *head, int clvmd_cmd, const char *node,
			  int len)
{
	head->cmd = clvmd_cmd;
	head->status = 0;
	head->flags = 0;
	head->xid = 0;
	head->clientid = 0;
	head->arglen = len;

	/*
	 * Handle special node names.
	 */
	if (!node || !strcmp(node, NODE_ALL))
		head->node[0] = '\0';
	else if (!strcmp(node, NODE_LOCAL)) {
		head->node[0] = '\0';
		head->flags = CLVMD_FLAG_LOCAL;
	} else if (!strcmp(node, NODE_REMOTE)) {
		head->node[0] = '\0';
		head->flags = CLVMD_FLAG_REMOTE;
	} else
		strcpy(head->node, node);
}

/*
 * Send a message to a(or all) node(s) in the cluster and wait for replies
 */
static int _cluster_request(char clvmd_cmd, const char *node, void *data, int len,
			   lvm_response_t ** response, int *num)
{
	char outbuf[sizeof(struct clvm_header) + len + strlen(node) + 1] __attribute__((aligned(8)));
	char *inptr;
	char *retbuf = NULL;
	int status;
	int i;
	int num_responses = 0;
	struct clvm_header *head = (struct clvm_header *) outbuf;
	lvm_response_t *rarray;

	*num = 0;

	if (_clvmd_sock == -1)
		_clvmd_sock = _open_local_sock(0);

	if (_clvmd_sock == -1)
		return 0;

	/* 1 byte is used from struct clvm_header.args[1], so -> len - 1 */
	_build_header(head, clvmd_cmd, node, len - 1);
	memcpy(head->node + strlen(head->node) + 1, data, len);

	status = _send_request(outbuf, sizeof(struct clvm_header) +
			      strlen(head->node) + len - 1, &retbuf);
	if (!status)
		goto out;

	/* Count the number of responses we got */
	head = (struct clvm_header *) retbuf;
	inptr = head->args;
	while (inptr[0]) {
		num_responses++;
		inptr += strlen(inptr) + 1;
		inptr += sizeof(int);
		inptr += strlen(inptr) + 1;
	}

	/*
	 * Allocate response array.
	 * With an extra pair of INTs on the front to sanity
	 * check the pointer when we are given it back to free
	 */
	*response = NULL;
	if (!(rarray = dm_malloc(sizeof(lvm_response_t) * num_responses))) {
		errno = ENOMEM;
		status = 0;
		goto out;
	}

	/* Unpack the response into an lvm_response_t array */
	inptr = head->args;
	i = 0;
	while (inptr[0]) {
		strcpy(rarray[i].node, inptr);
		inptr += strlen(inptr) + 1;

		memcpy(&rarray[i].status, inptr, sizeof(int));
		inptr += sizeof(int);

		rarray[i].response = dm_malloc(strlen(inptr) + 1);
		if (rarray[i].response == NULL) {
			/* Free up everything else and return error */
			int j;
			for (j = 0; j < i; j++)
				dm_free(rarray[i].response);
			dm_free(rarray);
			errno = ENOMEM;
			status = 0;
			goto out;
		}

		strcpy(rarray[i].response, inptr);
		rarray[i].len = strlen(inptr);
		inptr += strlen(inptr) + 1;
		i++;
	}
	*num = num_responses;
	*response = rarray;

      out:
	dm_free(retbuf);

	return status;
}

/* Free reply array */
static int _cluster_free_request(lvm_response_t * response, int num)
{
	int i;

	for (i = 0; i < num; i++) {
		dm_free(response[i].response);
	}

	dm_free(response);

	return 1;
}

static int _lock_for_cluster(struct cmd_context *cmd, unsigned char clvmd_cmd,
			     uint32_t flags, const char *name)
{
	int status;
	int i;
	char *args;
	const char *node = "";
	int len;
	int dmeventd_mode;
	int saved_errno;
	lvm_response_t *response = NULL;
	int num_responses;

	assert(name);

	len = strlen(name) + 3;
	args = alloca(len);
	strcpy(args + 2, name);

	/* args[0] holds bottom 8 bits except LCK_LOCAL (0x40). */
	args[0] = flags & (LCK_SCOPE_MASK | LCK_TYPE_MASK | LCK_NONBLOCK | LCK_HOLD | LCK_CLUSTER_VG); 

	args[1] = 0;

	if (flags & LCK_ORIGIN_ONLY)
		args[1] |= LCK_ORIGIN_ONLY_MODE;

	if (flags & LCK_REVERT)
		args[1] |= LCK_REVERT_MODE;

	if (mirror_in_sync())
		args[1] |= LCK_MIRROR_NOSYNC_MODE;

	if (test_mode())
		args[1] |= LCK_TEST_MODE;

	/*
	 * We propagate dmeventd_monitor_mode() to clvmd faithfully, since
	 * dmeventd monitoring is tied to activation which happens inside clvmd
	 * when locking_type = 3.
	 */
	dmeventd_mode = dmeventd_monitor_mode();
	if (dmeventd_mode == DMEVENTD_MONITOR_IGNORE)
		args[1] |= LCK_DMEVENTD_MONITOR_IGNORE;

	if (dmeventd_mode)
		args[1] |= LCK_DMEVENTD_MONITOR_MODE;

	if (cmd->partial_activation)
		args[1] |= LCK_PARTIAL_MODE;

	/*
	 * VG locks are just that: locks, and have no side effects
	 * so we only need to do them on the local node because all
	 * locks are cluster-wide.
	 *
	 * P_ locks /do/ get distributed across the cluster because they might
	 * have side-effects.
	 *
	 * SYNC_NAMES and VG_BACKUP use the VG name directly without prefix.
	 */
	if (clvmd_cmd == CLVMD_CMD_SYNC_NAMES) {
		if (flags & LCK_LOCAL)
			node = NODE_LOCAL;
	} else if (clvmd_cmd != CLVMD_CMD_VG_BACKUP) {
		if (strncmp(name, "P_", 2) &&
		    (clvmd_cmd == CLVMD_CMD_LOCK_VG ||
		     (flags & LCK_LOCAL) ||
		     !(flags & LCK_CLUSTER_VG)))
			node = NODE_LOCAL;
		else if (flags & LCK_REMOTE)
			node = NODE_REMOTE;
	}

	status = _cluster_request(clvmd_cmd, node, args, len,
				  &response, &num_responses);

	/* If any nodes were down then display them and return an error */
	for (i = 0; i < num_responses; i++) {
		if (response[i].status == EHOSTDOWN) {
			log_error("clvmd not running on node %s",
				  response[i].node);
			status = 0;
			errno = response[i].status;
		} else if (response[i].status) {
			log_error("Error locking on node %s: %s",
				  response[i].node,
				  response[i].response[0] ?
				  	response[i].response :
				  	strerror(response[i].status));
			status = 0;
			errno = response[i].status;
		}
	}

	saved_errno = errno;
	_cluster_free_request(response, num_responses);
	errno = saved_errno;

	return status;
}

/* API entry point for LVM */
#ifdef CLUSTER_LOCKING_INTERNAL
static int _lock_resource(struct cmd_context *cmd, const char *resource,
			  uint32_t flags, struct logical_volume *lv __attribute__((unused)))
#else
	int lock_resource(struct cmd_context *cmd, const char *resource, uint32_t flags, struct logical_volume *lv __attribute__((unused)))
#endif
{
	char lockname[PATH_MAX];
	int clvmd_cmd = 0;
	const char *lock_scope;
	const char *lock_type = "";

	assert(strlen(resource) < sizeof(lockname));
	assert(resource);

	switch (flags & LCK_SCOPE_MASK) {
	case LCK_VG:
		if (!strcmp(resource, VG_SYNC_NAMES)) {
			log_very_verbose("Requesting sync names.");
			return _lock_for_cluster(cmd, CLVMD_CMD_SYNC_NAMES,
						 flags & ~LCK_HOLD, resource);
		}
		if (flags == LCK_VG_BACKUP) {
			log_very_verbose("Requesting backup of VG metadata for %s",
					 resource);
			return _lock_for_cluster(cmd, CLVMD_CMD_VG_BACKUP,
						 LCK_CLUSTER_VG, resource);
		}

		/* If the VG name is empty then lock the unused PVs */
		if (dm_snprintf(lockname, sizeof(lockname), "%c_%s",
				(is_orphan_vg(resource) ||
				 is_global_vg(resource) ||
				 (flags & LCK_CACHE)) ?  'P' : 'V',
				resource)  < 0) {
			log_error("Locking resource %s too long.", resource);
			return 0;
		}

		lock_scope = "VG";
		clvmd_cmd = CLVMD_CMD_LOCK_VG;
		/*
		 * Old clvmd does not expect LCK_HOLD which was already processed
		 * in lock_vol, mask it for compatibility reasons.
		 */
		if (flags != LCK_VG_COMMIT && flags != LCK_VG_REVERT)
			flags &= ~LCK_HOLD;

		break;

	case LCK_LV:
		clvmd_cmd = CLVMD_CMD_LOCK_LV;
		strcpy(lockname, resource);
		lock_scope = "LV";
		flags &= ~LCK_HOLD;	/* Mask off HOLD flag */
		break;

	default:
		log_error("Unrecognised lock scope: %d",
			  flags & LCK_SCOPE_MASK);
		return 0;
	}

	switch(flags & LCK_TYPE_MASK) {
	case LCK_UNLOCK:
		lock_type = "UN";
		break;
	case LCK_NULL:
		lock_type = "NL";
		break;
	case LCK_READ:
		lock_type = "CR";
		break;
	case LCK_PREAD:
		lock_type = "PR";
		break;
	case LCK_WRITE:
		lock_type = "PW";
		break;
	case LCK_EXCL:
		lock_type = "EX";
		break;
	default:
		log_error("Unrecognised lock type: %u",
			  flags & LCK_TYPE_MASK);
		return 0;
	}

	log_very_verbose("Locking %s %s %s (%s%s%s%s%s%s%s%s%s) (0x%x)", lock_scope, lockname,
			 lock_type, lock_scope,
			 flags & LCK_NONBLOCK ? "|NONBLOCK" : "",
			 flags & LCK_HOLD ? "|HOLD" : "",
			 flags & LCK_CLUSTER_VG ? "|CLUSTER" : "",
			 flags & LCK_LOCAL ? "|LOCAL" : "",
			 flags & LCK_REMOTE ? "|REMOTE" : "",
			 flags & LCK_CACHE ? "|CACHE" : "",
			 flags & LCK_ORIGIN_ONLY ? "|ORIGIN_ONLY" : "",
			 flags & LCK_REVERT ? "|REVERT" : "",
			 flags);

	/* Send a message to the cluster manager */
	return _lock_for_cluster(cmd, clvmd_cmd, flags, lockname);
}

static int decode_lock_type(const char *response)
{
	if (!response)
		return LCK_NULL;
	else if (!strcmp(response, "EX"))
		return LCK_EXCL;
	else if (!strcmp(response, "CR"))
		return LCK_READ;
	else if (!strcmp(response, "PR"))
		return LCK_PREAD;

	return_0;
}

#ifdef CLUSTER_LOCKING_INTERNAL
static int _query_resource(const char *resource, int *mode)
#else
int query_resource(const char *resource, int *mode)
#endif
{
	int i, status, len, num_responses, saved_errno;
	const char *node = "";
	char *args;
	lvm_response_t *response = NULL;

	saved_errno = errno;
	len = strlen(resource) + 3;
	args = alloca(len);
	strcpy(args + 2, resource);

	args[0] = 0;
	args[1] = 0;

	status = _cluster_request(CLVMD_CMD_LOCK_QUERY, node, args, len,
				  &response, &num_responses);
	*mode = LCK_NULL;
	for (i = 0; i < num_responses; i++) {
		if (response[i].status == EHOSTDOWN)
			continue;

		if (!response[i].response[0])
			continue;

		/*
		 * All nodes should use CR, or exactly one node
		 * should hold EX. (PR is obsolete)
		 * If two nodes report different locks,
		 * something is broken - just return more important mode.
		 */
		if (decode_lock_type(response[i].response) > *mode)
			*mode = decode_lock_type(response[i].response);

		log_debug_locking("Lock held for %s, node %s : %s", resource,
				  response[i].node, response[i].response);
	}

	_cluster_free_request(response, num_responses);
	errno = saved_errno;

	return status;
}

#ifdef CLUSTER_LOCKING_INTERNAL
static void _locking_end(void)
#else
void locking_end(void)
#endif
{
	if (_clvmd_sock != -1 && close(_clvmd_sock))
		stack;

	_clvmd_sock = -1;
}

#ifdef CLUSTER_LOCKING_INTERNAL
static void _reset_locking(void)
#else
void reset_locking(void)
#endif
{
	if (close(_clvmd_sock))
		stack;

	_clvmd_sock = _open_local_sock(0);
	if (_clvmd_sock == -1)
		stack;
}

#ifdef CLUSTER_LOCKING_INTERNAL
int init_cluster_locking(struct locking_type *locking, struct cmd_context *cmd,
			 int suppress_messages)
{
	locking->lock_resource = _lock_resource;
	locking->query_resource = _query_resource;
	locking->fin_locking = _locking_end;
	locking->reset_locking = _reset_locking;
	locking->flags = LCK_PRE_MEMLOCK | LCK_CLUSTERED;

	_clvmd_sock = _open_local_sock(suppress_messages);
	if (_clvmd_sock == -1)
		return 0;

	return 1;
}
#else
int locking_init(int type, struct dm_config_tree *cf, uint32_t *flags)
{
	_clvmd_sock = _open_local_sock(0);
	if (_clvmd_sock == -1)
		return 0;

	/* Ask LVM to lock memory before calling us */
	*flags |= LCK_PRE_MEMLOCK;
	*flags |= LCK_CLUSTERED;

	return 1;
}
#endif
