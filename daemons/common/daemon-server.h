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

#include "daemon-client.h"
#include "config.h" // XXX will be in libdevmapper.h later

#ifndef _LVM_DAEMON_COMMON_SERVER_H
#define _LVM_DAEMON_COMMON_SERVER_H

typedef struct {
	int socket_fd; /* the fd we use to talk to the client */
	pthread_t thread_id;
	char *read_buf;
	void *private; /* this holds per-client state */
} client_handle;

typedef struct {
	struct config_tree *cft;
	char *buffer;
} request;

typedef struct {
	int error;
	struct config_tree *cft;
	char *buffer;
} response;

struct daemon_state;

/*
 * Craft a simple reply, without the need to construct a config_tree. See
 * daemon_send_simple in daemon-client.h for the description of the parameters.
 */
response daemon_reply_simple(char *id, ...);

static inline int daemon_request_int(request r, const char *path, int def) {
	if (!r.cft)
		return def;
	return find_config_int(r.cft->root, path, def);
}

static inline const char *daemon_request_str(request r, const char *path, const char *def) {
	if (!r.cft)
		return def;
	return find_config_str(r.cft->root, path, def);
}

/*
 * The callback. Called once per request issued, in the respective client's
 * thread. It is presented by a parsed request (in the form of a config tree).
 * The output is a new config tree that is serialised and sent back to the
 * client. The client blocks until the request processing is done and reply is
 * sent.
 */
typedef response (*handle_request)(struct daemon_state s, client_handle h, request r);

typedef struct daemon_state {
	/*
	 * The maximal stack size for individual daemon threads. This is
	 * essential for daemons that need to be locked into memory, since
	 * pthread's default is 10M per thread.
	 */
	int thread_stack_size;

	/* Flags & attributes affecting the behaviour of the daemon. */
	unsigned avoid_oom:1;
	unsigned foreground:1;
	const char *name;
	const char *pidfile;
	const char *socket_path;
	int log_level;
	handle_request handler;
	int (*daemon_init)(struct daemon_state *st);
	int (*daemon_fini)(struct daemon_state *st);

	/* Global runtime info maintained by the framework. */
	int socket_fd;

	void *private; /* the global daemon state */
} daemon_state;

/*
 * Start serving the requests. This does all the daemonisation, socket setup
 * work and so on. This function takes over the process, and upon failure, it
 * will terminate execution. It may be called at most once.
 */
void daemon_start(daemon_state s);

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
