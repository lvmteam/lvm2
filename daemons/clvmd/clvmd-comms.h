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
 * Abstraction layer for clvmd cluster communications
 */

#ifndef _CLVMD_COMMS_H
#define _CLVMD_COMMS_H

struct local_client;

extern int cluster_send_message(void *buf, int msglen, char *csid,
				const char *errtext);
extern int name_from_csid(char *csid, char *name);
extern int csid_from_name(char *csid, char *name);
extern int get_num_nodes(void);
extern int cluster_fd_callback(struct local_client *fd, char *buf, int len,
			       char *csid, struct local_client **new_client);
extern int init_cluster(void);
extern int get_main_cluster_fd(void);	/* gets accept FD or cman cluster socket */
extern int cluster_do_node_callback(struct local_client *client,
				    void (*callback) (struct local_client *,
						      char *csid, int node_up));
extern int is_quorate(void);

extern void get_our_csid(char *csid);
extern void add_up_node(char *csid);
extern void cluster_closedown(void);

extern int sync_lock(const char *resource, int mode, int flags, int *lockid);
extern int sync_unlock(const char *resource, int lockid);

#ifdef USE_GULM
#include "tcp-comms.h"
#else
/* cman */
#include "cnxman-socket.h"
#define MAX_CSID_LEN 4
#endif


#endif
