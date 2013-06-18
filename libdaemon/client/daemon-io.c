/*
 * Copyright (C) 2011-2013 Red Hat, Inc.
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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "daemon-io.h"
#include "libdevmapper.h"

/*
 * Read a single message from a (socket) filedescriptor. Messages are delimited
 * by blank lines. This call will block until all of a message is received. The
 * memory will be allocated from heap. Upon error, all memory is freed and the
 * buffer pointer is set to NULL.
 *
 * See also write_buffer about blocking (read_buffer has identical behaviour).
 */
int buffer_read(int fd, struct buffer *buffer) {
	int result;

	if (!buffer_realloc(buffer, 32)) /* ensure we have some space */
		return 0;

	while (1) {
		result = read(fd, buffer->mem + buffer->used, buffer->allocated - buffer->used);
		if (result > 0) {
			buffer->used += result;
			if (!strncmp((buffer->mem) + buffer->used - 4, "\n##\n", 4)) {
				buffer->used -= 4;
				buffer->mem[buffer->used] = 0;
				break; /* success, we have the full message now */
			}
			if ((buffer->allocated - buffer->used < 32) &&
			    !buffer_realloc(buffer, 1024))
				return 0;
		} else if (result == 0) {
			errno = ECONNRESET;
			return 0; /* we should never encounter EOF here */
		} else if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			return 0;
		/* TODO call select here if we encountered EAGAIN/EWOULDBLOCK/EINTR */
	}

	return 1;
}

/*
 * Write a buffer to a filedescriptor. Keep trying. Blocks (even on
 * SOCK_NONBLOCK) until all of the write went through.
 *
 * TODO use select on EWOULDBLOCK/EAGAIN/EINTR to avoid useless spinning
 */
int buffer_write(int fd, const struct buffer *buffer) {
	static const struct buffer _terminate = { .mem = (char *) "\n##\n", .used = 4 };
	const struct buffer *use;
	int done, written, result;

	for (done = 0; done < 2; ++done) {
		use = (done == 0) ? buffer : &_terminate;
		for (written = 0; written < use->used;) {
			result = write(fd, use->mem + written, use->used - written);
			if (result > 0)
				written += result;
			else if (result < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
				return 0; /* too bad */
		}
	}

	return 1;
}
