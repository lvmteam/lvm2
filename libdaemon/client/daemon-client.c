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

#include "daemon-io.h"
#include "config-util.h"
#include "daemon-client.h"
#include "dm-logging.h"

#include <sys/un.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h> // ENOMEM

daemon_handle daemon_open(daemon_info i)
{
	daemon_handle h = { .protocol_version = 0, .error = 0 };
	daemon_reply r = { 0 };
	struct sockaddr_un sockaddr = { .sun_family = AF_UNIX };

	log_debug("%s: Opening daemon socket to %s for protocol %s version %d.",
		  i.socket, i.path, i.protocol, i.protocol_version);

	if ((h.socket_fd = socket(PF_UNIX, SOCK_STREAM /* | SOCK_NONBLOCK */, 0)) < 0) {
		h.error = errno;
		log_sys_error("socket", i.socket);
		goto error;
	}

	if (!dm_strncpy(sockaddr.sun_path, i.socket, sizeof(sockaddr.sun_path))) {
		log_error("%s: Daemon socket path too long.", i.socket);
		goto error;
	}

	if (connect(h.socket_fd,(struct sockaddr *) &sockaddr, sizeof(sockaddr))) {
		h.error = errno;
		log_sys_error("connect", i.socket);
		goto error;
	}

	log_debug("Sending daemon %s: hello", i.path);
	r = daemon_send_simple(h, "hello", NULL);
	if (r.error || strcmp(daemon_reply_str(r, "response", "unknown"), "OK")) {
		h.error = r.error;
		log_error("Daemon %s returned error %d", i.path, r.error);
		goto error;
	}

	/* Check protocol and version matches */
	h.protocol = daemon_reply_str(r, "protocol", NULL);
	if (h.protocol)
		h.protocol = dm_strdup(h.protocol); /* keep around */
	h.protocol_version = daemon_reply_int(r, "version", 0);

	if (i.protocol && (!h.protocol || strcmp(h.protocol, i.protocol))) {
		log_error("Daemon %s: requested protocol %s != %s",
			i.path, i.protocol, h.protocol ? : "");
		goto error;
	}
	if (i.protocol_version && h.protocol_version != i.protocol_version) {
		log_error("Daemon %s: requested protocol version %d != %d",
			  i.path, i.protocol_version, h.protocol_version);
		goto error;
	}

	daemon_reply_destroy(r);
	return h;

error:
	if (h.socket_fd >= 0)
		if (close(h.socket_fd))
			log_sys_error("close", "daemon_open");
	if (r.cft)
		daemon_reply_destroy(r);
	h.socket_fd = -1;
	return h;
}

daemon_reply daemon_send(daemon_handle h, daemon_request rq)
{
	struct buffer buffer;
	daemon_reply reply = { 0 };
	assert(h.socket_fd >= 0);
	buffer = rq.buffer;

	if (!buffer.mem)
		if (!dm_config_write_node(rq.cft->root, buffer_line, &buffer)) {
			reply.error = ENOMEM;
			return reply;
		}

	assert(buffer.mem);
	if (!buffer_write(h.socket_fd, &buffer))
		reply.error = errno;

	if (buffer_read(h.socket_fd, &reply.buffer)) {
		reply.cft = dm_config_from_string(reply.buffer.mem);
		if (!reply.cft)
			reply.error = EPROTO;
	} else
		reply.error = errno;

	if (buffer.mem != rq.buffer.mem)
		buffer_destroy(&buffer);

	return reply;
}

void daemon_reply_destroy(daemon_reply r) {
	if (r.cft)
		dm_config_destroy(r.cft);
	buffer_destroy(&r.buffer);
}

daemon_reply daemon_send_simple_v(daemon_handle h, const char *id, va_list ap)
{
	static const daemon_reply err = { .error = ENOMEM };
	daemon_request rq = { .cft = NULL };
	daemon_reply repl;

	if (!buffer_append_f(&rq.buffer, "request = %s", id, NULL) ||
	    !buffer_append_vf(&rq.buffer, ap)) {
		buffer_destroy(&rq.buffer);
		return err;
	}

	repl = daemon_send(h, rq);
	buffer_destroy(&rq.buffer);

	return repl;
}

daemon_reply daemon_send_simple(daemon_handle h, const char *id, ...)
{
	daemon_reply r;
	va_list ap;

	va_start(ap, id);
	r = daemon_send_simple_v(h, id, ap);
	va_end(ap);

	return r;
}

void daemon_close(daemon_handle h)
{
	dm_free((char *)h.protocol);
}

daemon_request daemon_request_make(const char *id)
{
	daemon_request r;
	r.cft = NULL;
	buffer_init(&r.buffer);

	if (!(r.cft = dm_config_create()))
		goto bad;

	if (!(r.cft->root = make_text_node(r.cft, "request", id, NULL, NULL)))
		goto bad;

	return r;
bad:
	if (r.cft)
		dm_config_destroy(r.cft);
	r.cft = NULL;
	return r;
}

int daemon_request_extend_v(daemon_request r, va_list ap)
{
	if (!r.cft)
		return 0;

	if (!config_make_nodes_v(r.cft, NULL, r.cft->root, ap))
		return 0;

	return 1;
}

int daemon_request_extend(daemon_request r, ...)
{
	int res;
	va_list ap;

	va_start(ap, r);
	res = daemon_request_extend_v(r, ap);
	va_end(ap);

	return res;
}

void daemon_request_destroy(daemon_request r) {
	if (r.cft)
		dm_config_destroy(r.cft);
	buffer_destroy(&r.buffer);
}

