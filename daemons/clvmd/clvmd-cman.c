/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * CMAN communication layer for clvmd.
 */

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>

#include "clvmd-comms.h"
#include "clvm.h"
#include "libdlm.h"
#include "log.h"
#include "clvmd.h"
#include "lvm-functions.h"

#define LOCKSPACE_NAME "clvmd"

static int cluster_sock;
static int num_nodes;
static struct cl_cluster_node *nodes = NULL;
static int count_nodes; /* size of allocated nodes array */
static int max_updown_nodes = 50;	/* Current size of the allocated array */
/* Node up/down status, indexed by nodeid */
static int *node_updown = NULL;
static dlm_lshandle_t *lockspace;

static void sigusr1_handler(int sig);
static void count_clvmds_running(void);
static void get_members(void);
static int nodeid_from_csid(char *csid);
static int name_from_nodeid(int nodeid, char *name);

struct lock_wait {
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	struct dlm_lksb lksb;
};

int init_cluster()
{
	struct sockaddr_cl saddr;
	int port = CLUSTER_PORT_CLVMD;

	/* Open the cluster communication socket */
	cluster_sock = socket(AF_CLUSTER, SOCK_DGRAM, CLPROTO_CLIENT);
	if (cluster_sock == -1) {
		perror("Can't open cluster socket");
		return -1;
	}

	/* Bind to our port number on the cluster.
	   Writes to this will block if the cluster loses quorum */
	saddr.scl_family = AF_CLUSTER;
	saddr.scl_port = port;

	if (bind
	    (cluster_sock, (struct sockaddr *) &saddr,
	     sizeof(struct sockaddr_cl))) {
		log_error("Can't bind cluster socket: %m");
		return -1;
	}

	/* Get the cluster members list */
	get_members();
	count_clvmds_running();

	/* Create a lockspace for LV & VG locks to live in */
	lockspace = dlm_create_lockspace(LOCKSPACE_NAME, 0600);
	if (!lockspace) {
		log_error("Unable to create lockspace for CLVM\n");
		return -1;
	}
	dlm_ls_pthread_init(lockspace);
	return 0;
}

int get_main_cluster_fd()
{
	return cluster_sock;
}

int get_num_nodes()
{
	return num_nodes;
}

/* send_message with the fd check removed */
int cluster_send_message(void *buf, int msglen, char *csid, const char *errtext)
{
	struct iovec iov[2];
	struct msghdr msg;
	struct sockaddr_cl saddr;
	int len = 0;

	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_iovlen = 1;
	msg.msg_iov = iov;
	msg.msg_flags = 0;
	iov[0].iov_len = msglen;
	iov[0].iov_base = buf;

	saddr.scl_family = AF_CLUSTER;
	saddr.scl_port = CLUSTER_PORT_CLVMD;
	if (csid) {
		msg.msg_name = &saddr;
		msg.msg_namelen = sizeof(saddr);
		memcpy(&saddr.scl_nodeid, csid, MAX_CSID_LEN);
	} else {		/* Cluster broadcast */

		msg.msg_name = NULL;
		msg.msg_namelen = 0;
	}

	do {
		len = sendmsg(cluster_sock, &msg, 0);
		if (len < 0 && errno != EAGAIN)
			log_error(errtext);

	} while (len == -1 && errno == EAGAIN);
	return len;
}

void get_our_csid(char *csid)
{
	int i;
	memset(csid, 0, MAX_CSID_LEN);

	for (i = 0; i < num_nodes; i++) {
		if (nodes[i].us)
			memcpy(csid, &nodes[i].node_id, MAX_CSID_LEN);
	}
}

/* Call a callback routine for each node that known (down mean not running a clvmd) */
int cluster_do_node_callback(struct local_client *client,
			     void (*callback) (struct local_client *, char *,
					       int))
{
	int i;
	int somedown = 0;

	for (i = 0; i < get_num_nodes(); i++) {
		callback(client, (char *)&nodes[i].node_id, node_updown[nodes[i].node_id]);
		if (!node_updown[nodes[i].node_id])
			somedown = -1;
	}
	return somedown;
}

/* Process OOB message from the cluster socket,
   this currently just means that a node has stopped listening on our port */
static void process_oob_msg(char *buf, int len, int nodeid)
{
	char namebuf[256];
	switch (buf[0]) {
        case CLUSTER_OOB_MSG_PORTCLOSED:
		name_from_nodeid(nodeid, namebuf);
		log_notice("clvmd on node %s has died\n", namebuf);
		DEBUGLOG("Got OOB message, removing node %s\n", namebuf);

		node_updown[nodeid] = 0;
		break;

	case CLUSTER_OOB_MSG_STATECHANGE:
		DEBUGLOG("Got OOB message, Cluster state change\n");
		get_members();
		break;
	default:
		/* ERROR */
		DEBUGLOG("Got unknown OOB message: %d\n", buf[0]);
	}
}

int cluster_fd_callback(struct local_client *fd, char *buf, int len, char *csid,
			struct local_client **new_client)
{
	struct iovec iov[2];
	struct msghdr msg;
	struct sockaddr_cl saddr;

	/* We never return a new client */
	*new_client = NULL;

	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_iovlen = 1;
	msg.msg_iov = iov;
	msg.msg_name = &saddr;
	msg.msg_flags = 0;
	msg.msg_namelen = sizeof(saddr);
	iov[0].iov_len = len;
	iov[0].iov_base = buf;

	len = recvmsg(cluster_sock, &msg, MSG_OOB | O_NONBLOCK);
	if (len < 0 && errno == EAGAIN)
		return len;

	DEBUGLOG("Read on cluster socket, len = %d\n", len);

	/* A real error */
	if (len < 0) {
		log_error("read error on cluster socket: %m");
		return 0;
	}

	/* EOF - we have left the cluster */
	if (len == 0)
		return 0;

	/* Is it OOB? probably a node gone down */
	if (msg.msg_flags & MSG_OOB) {
		process_oob_msg(iov[0].iov_base, len, saddr.scl_nodeid);

		/* Tell the upper layer to ignore this message */
		len = -1;
		errno = EAGAIN;
	}
	memcpy(csid, &saddr.scl_nodeid, sizeof(saddr.scl_nodeid));
	return len;
}

void add_up_node(char *csid)
{
	/* It's up ! */
	int nodeid = nodeid_from_csid(csid);

	if (nodeid >= max_updown_nodes) {
		int *new_updown = realloc(node_updown, max_updown_nodes + 10);

		if (new_updown) {
			node_updown = new_updown;
			max_updown_nodes += 10;
			DEBUGLOG("realloced more space for nodes. now %d\n",
				 max_updown_nodes);
		} else {
			log_error
			    ("Realloc failed. Node status for clvmd will be wrong\n");
			return;
		}
	}
	node_updown[nodeid] = 1;
	DEBUGLOG("Added new node %d to updown list\n", nodeid);
}

void cluster_closedown()
{
	unlock_all();
	dlm_release_lockspace(LOCKSPACE_NAME, lockspace, 1);
	close(cluster_sock);
}

static int is_listening(int nodeid)
{
	struct cl_listen_request rq;
	int status;

	rq.port = CLUSTER_PORT_CLVMD;
	rq.nodeid = nodeid;

	do {
		status = ioctl(cluster_sock, SIOCCLUSTER_ISLISTENING, &rq);
		if (status < 0 && errno == EBUSY) {	/* Don't busywait */
			sleep(1);
			errno = EBUSY;	/* In case sleep trashes it */
		}
	}
	while (status < 0 && errno == EBUSY);

	return status;
}

/* Populate the list of CLVMDs running.
   called only at startup time */
void count_clvmds_running(void)
{
	int i;

	for (i = 0; i < num_nodes; i++) {
		node_updown[nodes[i].node_id] = is_listening(nodes[i].node_id);
	}
}

/* Get a list of active cluster members */
static void get_members()
{
	struct cl_cluster_nodelist nodelist;

	num_nodes = ioctl(cluster_sock, SIOCCLUSTER_GETMEMBERS, 0);
	if (num_nodes == -1) {
		perror("get nodes");
	} else {
	        /* Not enough room for new nodes list ? */
	        if (num_nodes > count_nodes && nodes) {
			free(nodes);
			nodes = NULL;
		}

		if (nodes == NULL) {
		        count_nodes = num_nodes + 10; /* Overallocate a little */
		        nodes = malloc(count_nodes * sizeof(struct cl_cluster_node));
			if (!nodes) {
			        perror("Unable to allocate nodes array\n");
				exit(5);
			}
		}
		nodelist.max_members = count_nodes;
		nodelist.nodes = nodes;
		
		num_nodes = ioctl(cluster_sock, SIOCCLUSTER_GETMEMBERS, &nodelist);
		if (num_nodes <= 0) {
		        perror("get node details");
			exit(6);
		}

		/* Sanity check struct */
		if (nodes[0].size != sizeof(struct cl_cluster_node)) {
			log_error
			    ("sizeof(cl_cluster_node) does not match size returned from the kernel: aborting\n");
			exit(10);
		}

		if (node_updown == NULL) {
			node_updown =
			    (int *) malloc(sizeof(int) *
					   max(num_nodes, max_updown_nodes));
			memset(node_updown, 0,
			       sizeof(int) * max(num_nodes, max_updown_nodes));
		}
	}
}

/* Convert a node name to a CSID */
int csid_from_name(char *csid, char *name)
{
	int i;

	for (i = 0; i < num_nodes; i++) {
		if (strcmp(name, nodes[i].name) == 0) {
			memcpy(csid, &nodes[i].node_id, MAX_CSID_LEN);
			return 0;
		}
	}
	return -1;
}

/* Convert a CSID to a node name */
int name_from_csid(char *csid, char *name)
{
	int i;

	for (i = 0; i < num_nodes; i++) {
		if (memcmp(csid, &nodes[i].node_id, MAX_CSID_LEN) == 0) {
			strcpy(name, nodes[i].name);
			return 0;
		}
	}
	/* Who?? */
	strcpy(name, "Unknown");
	return -1;
}

/* Convert a node ID to a node name */
int name_from_nodeid(int nodeid, char *name)
{
	int i;

	for (i = 0; i < num_nodes; i++) {
		if (nodeid == nodes[i].node_id) {
			strcpy(name, nodes[i].name);
			return 0;
		}
	}
	/* Who?? */
	strcpy(name, "Unknown");
	return -1;
}

/* Convert a CSID to a node ID */
static int nodeid_from_csid(char *csid)
{
        int nodeid;

	memcpy(&nodeid, csid, MAX_CSID_LEN);

	return nodeid;
}

int is_quorate()
{
	return ioctl(cluster_sock, SIOCCLUSTER_ISQUORATE, 0);
}

static void sync_ast_routine(void *arg)
{
	struct lock_wait *lwait = arg;

	pthread_mutex_lock(&lwait->mutex);
	pthread_cond_signal(&lwait->cond);
	pthread_mutex_unlock(&lwait->mutex);
}

int sync_lock(const char *resource, int mode, int flags, int *lockid)
{
	int status;
	struct lock_wait lwait;

	if (!lockid) {
		errno = EINVAL;
		return -1;
	}

	/* Conversions need the lockid in the LKSB */
	if (flags & LKF_CONVERT)
		lwait.lksb.sb_lkid = *lockid;

	pthread_cond_init(&lwait.cond, NULL);
	pthread_mutex_init(&lwait.mutex, NULL);
	pthread_mutex_lock(&lwait.mutex);

	status = dlm_ls_lock(lockspace,
			     mode,
			     &lwait.lksb,
			     flags,
			     resource,
			     strlen(resource),
			     0, sync_ast_routine, &lwait, NULL, NULL);
	if (status)
		return status;

	/* Wait for it to complete */
	pthread_cond_wait(&lwait.cond, &lwait.mutex);
	pthread_mutex_unlock(&lwait.mutex);

	*lockid = lwait.lksb.sb_lkid;

	errno = lwait.lksb.sb_status;
	if (lwait.lksb.sb_status)
		return -1;
	else
		return 0;
}

int sync_unlock(const char *resource /* UNUSED */, int lockid)
{
	int status;
	struct lock_wait lwait;

	pthread_cond_init(&lwait.cond, NULL);
	pthread_mutex_init(&lwait.mutex, NULL);
	pthread_mutex_lock(&lwait.mutex);

	status = dlm_ls_unlock(lockspace, lockid, 0, &lwait.lksb, &lwait);

	if (status)
		return status;

	/* Wait for it to complete */
	pthread_cond_wait(&lwait.cond, &lwait.mutex);
	pthread_mutex_unlock(&lwait.mutex);

	errno = lwait.lksb.sb_status;
	if (lwait.lksb.sb_status != EUNLOCK)
		return -1;
	else
		return 0;

}
