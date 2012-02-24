#include "daemon-client.h"
#include "daemon-shared.h"
#include <sys/un.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h> // ENOMEM

daemon_handle daemon_open(daemon_info i) {
	daemon_handle h = { .protocol_version = 0 };
	daemon_reply r = { .cft = NULL };
	struct sockaddr_un sockaddr;

	if ((h.socket_fd = socket(PF_UNIX, SOCK_STREAM /* | SOCK_NONBLOCK */, 0)) < 0) {
		perror("socket");
		goto error;
	}
	memset(&sockaddr, 0, sizeof(sockaddr));
	fprintf(stderr, "[C] connecting to %s\n", i.socket);
	strcpy(sockaddr.sun_path, i.socket);
	sockaddr.sun_family = AF_UNIX;
	if (connect(h.socket_fd,(struct sockaddr *) &sockaddr, sizeof(sockaddr))) {
		perror("connect");
		goto error;
	}

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
	if (h.socket_fd >= 0)
		close(h.socket_fd);
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
	write_buffer(h.socket_fd, rq.buffer, strlen(rq.buffer));
	dm_free(rq.buffer);

	if (read_buffer(h.socket_fd, &reply.buffer)) {
		reply.cft = dm_config_from_string(reply.buffer);
	} else
		reply.error = 1;

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
	return repl;
}

void daemon_close(daemon_handle h)
{
	dm_free((char *)h.protocol);
}
