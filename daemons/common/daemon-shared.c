#include <errno.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include "daemon-shared.h"

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
	*buffer = malloc(buffersize + 1);

	while (1) {
		int result = read(fd, (*buffer) + bytes, buffersize - bytes);
		if (result > 0)
			bytes += result;
		if (result == 0)
			goto fail; /* we should never encounter EOF here */
		if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			goto fail;

		if (bytes == buffersize) {
			buffersize += 1024;
			char *new = realloc(*buffer, buffersize + 1);
			if (new)
				*buffer = new;
			else
				goto fail;
		} else {
			(*buffer)[bytes] = 0;
			char *end;
			if ((end = strstr((*buffer) + bytes - 2, "\n\n"))) {
				*end = 0;
				break; /* success, we have the full message now */
			}
			/* TODO call select here if we encountered EAGAIN/EWOULDBLOCK */
		}
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
int write_buffer(int fd, char *buffer, int length) {
	int written = 0;
	while (1) {
		int result = write(fd, buffer + written, length - written);
		if (result > 0)
			written += result;
		if (result < 0 && errno != EWOULDBLOCK && errno != EAGAIN)
			break; /* too bad */
		if (written == length)
			return 1; /* done */
	}
	return 0;
}

char *format_buffer(const char *what, const char *id, va_list ap)
{
	char *buffer, *old;
	char *next;
	char *format;

	dm_asprintf(&buffer, "%s = \"%s\"\n", what, id);
	if (!buffer) goto fail;

	while (next = va_arg(ap, char *)) {
		old = buffer;
		if (strstr(next, "%d") || strstr(next, "%s")) {
			dm_asprintf(&format, "%%s%s\n", next);
			if (!format) goto fail;

			if (strstr(format, "%d"))
				dm_asprintf(&buffer, format, buffer, va_arg(ap, int));
			else
				dm_asprintf(&buffer, format, buffer, va_arg(ap, char *));

			dm_free(format);
			dm_free(old);
			if (!buffer) goto fail;
		} else {
			dm_asprintf(&buffer, "%s%s", buffer, next);
			dm_free(old);
			if (!buffer) goto fail;
		}
	}

	old = buffer;
	dm_asprintf(&buffer, "%s\n", buffer);
	dm_free(old);

	return buffer;
fail:
	dm_free(buffer);
	return NULL;
}
