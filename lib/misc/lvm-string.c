/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "lvm-types.h"
#include "lvm-string.h"

#include <ctype.h>

/*
 * On error, up to glibc 2.0.6, snprintf returned -1 if buffer was too small;
 * From glibc 2.1 it returns number of chars (excl. trailing null) that would 
 * have been written had there been room.
 *
 * lvm_snprintf reverts to the old behaviour.
 */
int lvm_snprintf(char *buf, size_t bufsize, const char *format, ...)
{
	int n;
	va_list ap;

	va_start(ap, format);
	n = vsnprintf(buf, bufsize, format, ap);
	va_end(ap);

	if (n < 0 || (n > bufsize - 1))
		return -1;

	return n;
}

int emit_to_buffer(char **buffer, size_t *size, const char *fmt, ...)
{
	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(*buffer, *size, fmt, ap);
	va_end(ap);

	if (n < 0 || (n == *size))
		return 0;

	*buffer += n;
	*size -= n;
	return 1;
}

/*
 * consume characters while they match the predicate function.
 */
static char *_consume(char *buffer, int (*fn) (int))
{
	while (*buffer && fn(*buffer))
		buffer++;

	return buffer;
}

static int _isword(int c)
{
	return !isspace(c);
}

/*
 * Split buffer into NULL-separated words in argv.
 * Returns number of words.
 */
int split_words(char *buffer, unsigned max, char **argv)
{
	unsigned arg;

	for (arg = 0; arg < max; arg++) {
		buffer = _consume(buffer, isspace);
		if (!*buffer)
			break;

		argv[arg] = buffer;
		buffer = _consume(buffer, _isword);

		if (*buffer) {
			*buffer = '\0';
			buffer++;
		}
	}

	return arg;
}
