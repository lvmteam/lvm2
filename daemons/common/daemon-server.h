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

#ifndef _LVM_DAEMON_COMMON_CLIENT_H
#define _LVM_DAEMON_COMMON_CLIENT_H

typedef struct {
	int socket_fd; /* the fd we use to talk to the client */
	pthread_t thread_id;
	char *read_buf;
	void *private; /* this holds per-client state */
} client_handle;

typedef struct {
	void *private; /* the global daemon state */
} daemon_state;

typedef struct {
	struct config_tree *cft;
} request;

typedef struct {
	struct config_tree *cft;
} response;

/*
 * The callback. Called once per request issued, in the respective client's
 * thread. It is presented by a parsed request (in the form of a config tree).
 * The output is a new config tree that is serialised and sent back to the
 * client. The client blocks until the request processing is done and reply is
 * sent.
 */
typedef response (*handle_request)(daemon_state s, client_handle h, request r);

/*
 * Start serving the requests. This does all the daemonisation, socket setup
 * work and so on.
 */
void daemon_start(daemon_state s, handle_request r);

/*
 * Take over from an already running daemon. This function handles connecting
 * to the running daemon and telling it we are going to take over. The takeover
 * request may be customised by passing in a non-NULL request.
 *
 * The takeover sequence: the old daemon stops accepting new clients, then it
 * waits until all current client connections are closed. When that happens, it
 * serializes its current state and sends that as a reply, which is then
 * returned by this function (therefore, this function won't return until the
 * previous instance has shut down).
 *
 * The daemon, after calling daemon_takeover is expected to set up its
 * daemon_state using the reply from this function and call daemon_start as
 * usual.
 */
daemon_reply daemon_takeover(daemon_info i, daemon_request r);

/* Call this to request a clean shutdown of the daemon. Async safe. */
void daemon_stop();

#endif
