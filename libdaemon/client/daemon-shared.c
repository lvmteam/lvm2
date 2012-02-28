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
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "daemon-shared.h"
#include "libdevmapper.h"

/*
 * Read a single message from a (socket) filedescriptor. Messages are delimited
 * by blank lines. This call will block until all of a message is received. The
 * memory will be allocated from heap. Upon error, all memory is freed and the
 * buffer pointer is set to NULL.
 *
 * See also write_buffer about blocking (read_buffer has identical behaviour).
 */
int read_buffer(int fd, char **buffer) {
	int bytes = 0;
	int buffersize = 32;
	char *new;
	*buffer = malloc(buffersize + 1);

	while (1) {
		int result = read(fd, (*buffer) + bytes, buffersize - bytes);
		if (result > 0)
			bytes += result;
		if (result == 0) {
			errno = ECONNRESET;
			goto fail; /* we should never encounter EOF here */
		}
		if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			goto fail;

		if (!strncmp((*buffer) + bytes - 4, "\n##\n", 4)) {
			*(*buffer + bytes - 4) = 0;
			break; /* success, we have the full message now */
		}

		if (bytes == buffersize) {
			buffersize += 1024;
			if (!(new = realloc(*buffer, buffersize + 1)))
				goto fail;

			*buffer = new;
		}
		/* TODO call select here if we encountered EAGAIN/EWOULDBLOCK */
	}
	return 1;
fail:
	free(*buffer);
	*buffer = NULL;
	return 0;
}

/*
 * Write a buffer to a filedescriptor. Keep trying. Blocks (even on
 * SOCK_NONBLOCK) until all of the write went through.
 *
 * TODO use select on EWOULDBLOCK/EAGAIN to avoid useless spinning
 */
int write_buffer(int fd, const char *buffer, int length) {
	static const char terminate[] = "\n##\n";
	int done = 0;
	int written = 0;
write:
	while (1) {
		int result = write(fd, buffer + written, length - written);
		if (result > 0)
			written += result;
		if (result < 0 && errno != EWOULDBLOCK && errno != EAGAIN)
			return 0; /* too bad */
		if (written == length) {
			if (done)
				return 1;
			else
				break; /* done */
		}
	}

	buffer = terminate;
	length = 4;
	written = 0;
	done = 1;
	goto write;
}

char *format_buffer(const char *what, const char *id, va_list ap)
{
	char *buffer, *old;
	char *next;
	int keylen;

	dm_asprintf(&buffer, "%s = \"%s\"\n", what, id);
	if (!buffer) goto fail;

	while ((next = va_arg(ap, char *))) {
		old = buffer;
		assert(strchr(next, '='));
		keylen = strchr(next, '=') - next;
		if (strstr(next, "%d")) {
			int value = va_arg(ap, int);
			dm_asprintf(&buffer, "%s%.*s= %d\n", buffer, keylen, next, value);
			dm_free(old);
		} else if (strstr(next, "%s")) {
			char *value = va_arg(ap, char *);
			dm_asprintf(&buffer, "%s%.*s= \"%s\"\n", buffer, keylen, next, value);
			dm_free(old);
		} else if (strstr(next, "%b")) {
			char *block = va_arg(ap, char *);
			if (!block)
				continue;
			dm_asprintf(&buffer, "%s%.*s%s", buffer, keylen, next, block);
			dm_free(old);
		} else {
			dm_asprintf(&buffer, "%s%s", buffer, next);
			dm_free(old);
		}
		if (!buffer) goto fail;
	}

	return buffer;
fail:
	dm_free(buffer);
	return NULL;
}
