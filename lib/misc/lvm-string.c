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
#include "pool.h"

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

/*
 * Device layer names are all of the form <vg>-<lv>-<layer>, any
 * other hyphens that appear in these names are quoted with yet
 * another hyphen.  The top layer of any device has no layer
 * name.  eg, vg0-lvol0.
 */
static void _count_hyphens(const char *str, size_t *len, int *hyphens)
{
	const char *ptr;

	for (ptr = str; *ptr; ptr++, (*len)++)
		if (*ptr == '-')
			(*hyphens)++;
}

/*
 * Copies a string, quoting hyphens with hyphens.
 */
static void _quote_hyphens(char **out, const char *src)
{
	while (*src) {
		if (*src == '-')
			*(*out)++ = '-';

		*(*out)++ = *src++;
	}
}

/*
 * <vg>-<lv>-<layer> or if !layer just <vg>-<lv>.
 */
char *build_dm_name(struct pool *mem, const char *vg,
		    const char *lv, const char *layer)
{
	size_t len = 0;
	int hyphens = 0;
	char *r, *out;

	_count_hyphens(vg, &len, &hyphens);
	_count_hyphens(lv, &len, &hyphens);

	if (layer && *layer)
		_count_hyphens(layer, &len, &hyphens);

	len += hyphens + 2;

	if (!(r = pool_alloc(mem, len))) {
		stack;
		return NULL;
	}

	out = r;
	_quote_hyphens(&out, vg);
	*out++ = '-';
	_quote_hyphens(&out, lv);

	if (layer && *layer) {
		*out++ = '-';
		_quote_hyphens(&out, layer);
	}
	*out = '\0';

	return r;
}

/*
 * Remove hyphen quoting from a component of a name.
 * NULL-terminates the component and returns start of next component.
 */
static char *_unquote(char *component)
{
	char *c = component;
	char *o = c;

	while (*c) {
		if (*(c + 1)) {
			if (*c == '-') {
				if (*(c + 1) == '-')
					c++;
				else
					break;
			}
		}
		*o = *c;
		o++;
		c++;
	}

	*o = '\0';
	return (c + 1);
}

int split_dm_name(struct pool *mem, const char *dmname,
		  char **vgname, char **lvname, char **layer)
{
	if (!(*vgname = pool_strdup(mem, dmname)))
		return 0;

	_unquote(*layer = _unquote(*lvname = _unquote(*vgname)));

	return 1;
}

