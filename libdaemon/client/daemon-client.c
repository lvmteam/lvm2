/*
 * Copyright (C) 2011-2012 Red Hat, Inc.
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

#include "daemon-shared.h"
#include "daemon-client.h"

#include <sys/un.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h> // ENOMEM

daemon_handle daemon_open(daemon_info i) {
	daemon_handle h = { .protocol_version = 0, .error = 0 };
	daemon_reply r = { .cft = NULL };
	struct sockaddr_un sockaddr;

	if ((h.socket_fd = socket(PF_UNIX, SOCK_STREAM /* | SOCK_NONBLOCK */, 0)) < 0)
		goto error;

	memset(&sockaddr, 0, sizeof(sockaddr));
	if (!dm_strncpy(sockaddr.sun_path, i.socket, sizeof(sockaddr.sun_path))) {
		fprintf(stderr, "%s: daemon socket path too long.\n", i.socket);
		goto error;
	}
	sockaddr.sun_family = AF_UNIX;
	if (connect(h.socket_fd,(struct sockaddr *) &sockaddr, sizeof(sockaddr)))
		goto error;

	r = daemon_send_simple(h, "hello", NULL);
	if (r.error || strcmp(daemon_reply_str(r, "response", "unknown"), "OK"))
		goto error;

	h.protocol = daemon_reply_str(r, "protocol", NULL);
	if (h.protocol)
		h.protocol = dm_strdup(h.protocol); /* keep around */
	h.protocol_version = daemon_reply_int(r, "version", 0);

	if (i.protocol && (!h.protocol || strcmp(h.protocol, i.protocol)))
		goto error;
	if (i.protocol_version && h.protocol_version != i.protocol_version)
		goto error;

	daemon_reply_destroy(r);
	return h;

error:
	h.error = errno;
	if (h.socket_fd >= 0)
		if (close(h.socket_fd))
			perror("close");
	if (r.cft)
		daemon_reply_destroy(r);
	h.socket_fd = -1;
	return h;
}

daemon_reply daemon_send(daemon_handle h, daemon_request rq)
{
	daemon_reply reply = { .cft = NULL, .error = 0 };
	assert(h.socket_fd >= 0);

	if (!rq.buffer) {
		/* TODO: build the buffer from rq.cft */
	}

	assert(rq.buffer);
	if (!write_buffer(h.socket_fd, rq.buffer, strlen(rq.buffer)))
		reply.error = errno;

	if (read_buffer(h.socket_fd, &reply.buffer)) {
		reply.cft = dm_config_from_string(reply.buffer);
	} else
		reply.error = errno;

	return reply;
}

void daemon_reply_destroy(daemon_reply r) {
	if (r.cft)
		dm_config_destroy(r.cft);
	dm_free(r.buffer);
}

daemon_reply daemon_send_simple(daemon_handle h, const char *id, ...)
{
	static const daemon_reply err = { .error = ENOMEM, .buffer = NULL, .cft = NULL };
	daemon_request rq = { .cft = NULL };
	daemon_reply repl;
	va_list ap;

	va_start(ap, id);
	rq.buffer = format_buffer("request", id, ap);
	va_end(ap);

	if (!rq.buffer)
		return err;

	repl = daemon_send(h, rq);
	dm_free(rq.buffer);

	return repl;
}

void daemon_close(daemon_handle h)
{
	dm_free((char *)h.protocol);
}
