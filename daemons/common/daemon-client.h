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

#include "libdevmapper.h" // for dm_list, needed by config.h
#include "config.h" // should become part of libdevmapper later

#ifndef _LVM_DAEMON_COMMON_CLIENT_H
#define _LVM_DAEMON_COMMON_CLIENT_H

typedef struct {
	int socket_fd; /* the fd we use to talk to the daemon */
	int protocol;  /* version of the protocol the daemon uses */
} daemon_handle;

typedef struct {
	const char *path; /* the binary of the daemon */
	const char *socket; /* path to the comms socket */
	unsigned autostart:1; /* start the daemon if not running? */
} daemon_info;

typedef struct {
	char *buffer;
	/*
	 * The request looks like this:
	 *    request = "id"
	 *    arg_foo = "something"
	 *    arg_bar = 3
	 *    arg_wibble {
	 *        something_special = "here"
	 *        amount = 75
	 *        knobs = [ "twiddle", "tweak" ]
	 *    }
	 */
	struct config_tree *cft;
} daemon_request;

typedef struct {
	int error; /* 0 for success */
	char *buffer; /* textual reply */
	struct config_tree *cft; /* parsed reply, if available */
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
 *
 * In case the request contains a non-NULL buffer pointer, this buffer is sent
 * *verbatim* to the server. In this case, the cft pointer may be NULL (but will
 * be ignored even if non-NULL). If the buffer is NULL, the cft is required to
 * be a valid pointer, and is used to build up the request.
 */
daemon_reply daemon_send(daemon_handle h, daemon_request r);

/*
 * A simple interface to daemon_send. This function just takes the command id
 * and possibly a list of parameters (of the form "name = %?", "value"). The
 * type (string, integer) of the value is indicated by a character substituted
 * for ? in %?: d for integer, s for string.
 */
daemon_reply daemon_send_simple(daemon_handle h, char *id, ...);

void daemon_reply_destroy(daemon_reply r);

static inline int daemon_reply_int(daemon_reply r, const char *path, int def) {
	return find_config_int(r.cft->root, path, def);
}

static inline const char *daemon_reply_str(daemon_reply r, const char *path, const char *def) {
	return find_config_str(r.cft->root, path, def);
}


/* Shut down the communication to the daemon. Compulsory. */
void daemon_close(daemon_handle h);

#endif
