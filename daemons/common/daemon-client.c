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
	daemon_handle h = { .protocol = 0 };
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
	return h;
error:
	if (h.socket_fd >= 0)
		close(h.socket_fd);
	h.socket_fd = -1;
	return h;
}

daemon_reply daemon_send(daemon_handle h, daemon_request rq)
{
	daemon_reply reply;
	assert(h.socket_fd >= 0);

	if (!rq.buffer) {
		/* TODO: build the buffer from rq.cft */
	}

	assert(rq.buffer);
	write_buffer(h.socket_fd, rq.buffer, strlen(rq.buffer));

	if (read_buffer(h.socket_fd, &reply.buffer)) {
		reply.cft = dm_config_from_string(reply.buffer);
	} else
		reply.error = 1;

	return reply;
}

void daemon_reply_destroy(daemon_reply r) {
	if (r.cft)
		dm_config_destroy(r.cft);
}

daemon_reply daemon_send_simple(daemon_handle h, char *id, ...)
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
}
