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
	if (!buffer_realloc(buffer, 32)) /* ensure we have some space */
		goto fail;

	while (1) {
		int result = read(fd, buffer->mem + buffer->used, buffer->allocated - buffer->used);
		if (result > 0) {
			buffer->used += result;
			if (!strncmp((buffer->mem) + buffer->used - 4, "\n##\n", 4)) {
				*(buffer->mem + buffer->used - 4) = 0;
				buffer->used -= 4;
				break; /* success, we have the full message now */
			}
			if (buffer->used - buffer->allocated < 32)
				if (!buffer_realloc(buffer, 1024))
					goto fail;
			continue;
		}
		if (result == 0) {
			errno = ECONNRESET;
			goto fail; /* we should never encounter EOF here */
		}
		if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			goto fail;
		/* TODO call select here if we encountered EAGAIN/EWOULDBLOCK/EINTR */
	}
	return 1;
fail:
	return 0;
}

/*
 * Write a buffer to a filedescriptor. Keep trying. Blocks (even on
 * SOCK_NONBLOCK) until all of the write went through.
 *
 * TODO use select on EWOULDBLOCK/EAGAIN/EINTR to avoid useless spinning
 */
int buffer_write(int fd, struct buffer *buffer) {
	struct buffer terminate = { .mem = (char *) "\n##\n", .used = 4 };
	int done = 0;
	int written = 0;
	struct buffer *use = buffer;
write:
	while (1) {
		int result = write(fd, use->mem + written, use->used - written);
		if (result > 0)
			written += result;
		if (result < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
			return 0; /* too bad */
		if (written == use->used) {
			if (done)
				return 1;
			else
				break; /* done */
		}
	}

	use = &terminate;
	written = 0;
	done = 1;
	goto write;
}
