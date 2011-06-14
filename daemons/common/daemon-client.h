/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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

#include "config.h" // should become part of libdevmapper later

#ifndef _LVM_DAEMON_COMMON_CLIENT_H
#define _LVM_DAEMON_COMMON_CLIENT_H

typedef struct {
	int socket_fd; /* the fd we use to talk to the daemon */
	int protocol;  /* version of the protocol the daemon uses */
	char *read_buf;
} daemon_handle;

typedef struct {
	const char *path; /* the binary of the daemon */
	const char *socket; /* path to the comms socket */
	unsigned autostart:1; /* start the daemon if not running? */
} daemon_info;

typedef struct {
	char *buffer;
	struct config_node *cft;
} daemon_request;

typedef struct {
	int error; /* 0 for success */
	char *buffer; /* textual reply */
	struct config_node *cft; /* parsed reply, if available */
} daemon_reply;

/*
 * Open the communication channel to the daemon. If the daemon is not running,
 * it may be autostarted based on the binary path provided in the info (this
 * will only happen if autostart is set to true). If the call fails for any
 * reason, daemon_handle_valid(h) for the response will return false. Otherwise,
 * the connection is good to start serving requests.
 */
daemon_handle daemon_open(daemon_info i);

/*
 * Send a request to the daemon, waiting for the reply. All communication with
 * the daemon is synchronous. The function handles the IO details and parses the
 * response, handling common error conditions. See "daemon_reply" for details.
 */
daemon_reply daemon_send(daemon_handle h, daemon_request r);

/* Shut down the communication to the daemon. Compulsory. */
void daemon_close(daemon_handle h);

#endif
