/*
 * Copyright (C) 1997-2004 Sistina Software, Inc. All rights reserved.
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

#ifndef _LIBCLVM_H
#define _LIBCLVM_H

typedef struct lvm_response {
	char node[255];
	char *response;
	int status;
	int len;

} lvm_response_t;

extern int lvm_cluster_request(char cmd, const char *node, void *data, int len,
			       lvm_response_t ** response, int *num);
extern int lvm_cluster_write(char cmd, char *node, void *data, int len);
extern int lvm_cluster_free_request(lvm_response_t * response);

/* The "high-level" API */
extern int lvm_lock_for_cluster(char scope, char *name, int verbosity);
extern int lvm_unlock_for_cluster(char scope, char *name, int verbosity);

#endif
