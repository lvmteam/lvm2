/*
 * Copyright (C) 2006-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "dmlib.h"

#include <ctype.h>

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
int dm_split_words(char *buffer, unsigned max,
		   unsigned ignore_comments __attribute__((unused)),
		   char **argv)
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
 * Remove hyphen quoting from a component of a name.
 * NULL-terminates the component and returns start of next component.
 */
static char *_unquote(char *component)
{
	char *c = component;
	char *o = c;
	char *r;

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

	r = (*c) ? c + 1 : c;
	*o = '\0';

	return r;
}

int dm_split_lvm_name(struct dm_pool *mem, const char *dmname,
		      char **vgname, char **lvname, char **layer)
{
	if (mem && !(*vgname = dm_pool_strdup(mem, dmname)))
		return 0;

	_unquote(*layer = _unquote(*lvname = _unquote(*vgname)));

	return 1;
}

/*
 * On error, up to glibc 2.0.6, snprintf returned -1 if buffer was too small;
 * From glibc 2.1 it returns number of chars (excl. trailing null) that would 
 * have been written had there been room.
 *
 * dm_snprintf reverts to the old behaviour.
 */
int dm_snprintf(char *buf, size_t bufsize, const char *format, ...)
{
	int n;
	va_list ap;

	va_start(ap, format);
	n = vsnprintf(buf, bufsize, format, ap);
	va_end(ap);

	if (n < 0 || ((unsigned) n + 1 > bufsize))
		return -1;

	return n;
}

const char *dm_basename(const char *path)
{
	const char *p = strrchr(path, '/');

	return p ? p + 1 : path;
}

int dm_asprintf(char **result, const char *format, ...)
{
	int n, ok = 0, size = 32;
	va_list ap;
	char *buf = dm_malloc(size);

	*result = 0;

	if (!buf)
		return -1;

	while (!ok) {
		va_start(ap, format);
		n = vsnprintf(buf, size, format, ap);
		va_end(ap);

		if (0 <= n && n < size)
			ok = 1;
		else {
			dm_free(buf);
			size *= 2;
			buf = dm_malloc(size);
			if (!buf)
				return -1;
		}
	}

	if (!(*result = dm_strdup(buf)))
		n = -2; /* return -1 */

	dm_free(buf);
	return n + 1;
}

/*
 * Count occurences of 'c' in 'str' until we reach a null char.
 *
 * Returns:
 *  len - incremented for each char we encounter.
 *  count - number of occurrences of 'c' and 'c2'.
 */
static void _count_chars(const char *str, size_t *len, int *count,
			 const int c1, const int c2)
{
	const char *ptr;

	for (ptr = str; *ptr; ptr++, (*len)++)
		if (*ptr == c1 || *ptr == c2)
			(*count)++;
}

/*
 * Count occurrences of 'c' in 'str' of length 'size'.
 *
 * Returns:
 *   Number of occurrences of 'c'
 */
unsigned dm_count_chars(const char *str, size_t len, const int c)
{
	size_t i;
	unsigned count = 0;

	for (i = 0; i < len; i++)
		if (str[i] == c)
			count++;

	return count;
}

/*
 * Length of string after escaping double quotes and backslashes.
 */
size_t dm_escaped_len(const char *str)
{
	size_t len = 1;
	int count = 0;

	_count_chars(str, &len, &count, '\"', '\\');

	return count + len;
}

/*
 * Copies a string, quoting orig_char with quote_char.
 * Optionally also quote quote_char.
 */
static void _quote_characters(char **out, const char *src,
			      const int orig_char, const int quote_char,
			      int quote_quote_char)
{
	while (*src) {
		if (*src == orig_char ||
		    (*src == quote_char && quote_quote_char))
			*(*out)++ = quote_char;

		*(*out)++ = *src++;
	}
}

static void _unquote_one_character(char *src, const char orig_char,
				   const char quote_char)
{
	char *out;
	char s, n;

	/* Optimise for the common case where no changes are needed. */
	while ((s = *src++)) {
		if (s == quote_char &&
		    ((n = *src) == orig_char || n == quote_char)) {
			out = src++;
			*(out - 1) = n;

			while ((s = *src++)) {
				if (s == quote_char &&
				    ((n = *src) == orig_char || n == quote_char)) {
					s = n;
					src++;
				}
				*out = s;
				out++;
			}

			*out = '\0';
			return;
		}
	}
}

/*
 * Unquote each character given in orig_char array and unquote quote_char
 * as well. Also save the first occurrence of each character from orig_char
 * that was found unquoted in arr_substr_first_unquoted array. This way we can
 * process several characters in one go.
 */
static void _unquote_characters(char *src, const char *orig_chars,
				size_t num_orig_chars,
				const char quote_char,
				char *arr_substr_first_unquoted[])
{
	char *out = src;
	char c, s, n;
	unsigned i;

	while ((s = *src++)) {
		for (i = 0; i < num_orig_chars; i++) {
			c = orig_chars[i];
			if (s == quote_char &&
			    ((n = *src) == c || n == quote_char)) {
				s = n;
				src++;
				break;
			}
			if (arr_substr_first_unquoted && (s == c) &&
			    !arr_substr_first_unquoted[i])
				arr_substr_first_unquoted[i] = out;
		};
		*out++ = s;
	}

	*out = '\0';
}

/*
 * Copies a string, quoting hyphens with hyphens.
 */
static void _quote_hyphens(char **out, const char *src)
{
	_quote_characters(out, src, '-', '-', 0);
}

/*
 * <vg>-<lv>-<layer> or if !layer just <vg>-<lv>.
 */
char *dm_build_dm_name(struct dm_pool *mem, const char *vgname,
		       const char *lvname, const char *layer)
{
	size_t len = 1;
	int hyphens = 1;
	char *r, *out;

	_count_chars(vgname, &len, &hyphens, '-', 0);
	_count_chars(lvname, &len, &hyphens, '-', 0);

	if (layer && *layer) {
		_count_chars(layer, &len, &hyphens, '-', 0);
		hyphens++;
	}

	len += hyphens;

	if (!(r = dm_pool_alloc(mem, len))) {
		log_error("build_dm_name: Allocation failed for %" PRIsize_t
			  " for %s %s %s.", len, vgname, lvname, layer);
		return NULL;
	}

	out = r;
	_quote_hyphens(&out, vgname);
	*out++ = '-';
	_quote_hyphens(&out, lvname);

	if (layer && *layer) {
		/* No hyphen if the layer begins with _ e.g. _mlog */
		if (*layer != '_')
			*out++ = '-';
		_quote_hyphens(&out, layer);
	}
	*out = '\0';

	return r;
}

char *dm_build_dm_uuid(struct dm_pool *mem, const char *uuid_prefix, const char *lvid, const char *layer)
{
	char *dmuuid;
	size_t len;

	if (!layer)
		layer = "";

	len = strlen(uuid_prefix) + strlen(lvid) + strlen(layer) + 2;

	if (!(dmuuid = dm_pool_alloc(mem, len))) {
		log_error("build_dm_name: Allocation failed for %" PRIsize_t
			  " %s %s.", len, lvid, layer);
		return NULL;
	}

	sprintf(dmuuid, "%s%s%s%s", uuid_prefix, lvid, (*layer) ? "-" : "", layer);

	return dmuuid;
}

/*
 * Copies a string, quoting double quotes with backslashes.
 */
char *dm_escape_double_quotes(char *out, const char *src)
{
	char *buf = out;

	_quote_characters(&buf, src, '\"', '\\', 1);
	*buf = '\0';

	return out;
}

/*
 * Undo quoting in situ.
 */
void dm_unescape_double_quotes(char *src)
{
	_unquote_one_character(src, '\"', '\\');
}

/*
 * Unescape colons and "at" signs in situ and save the substrings
 * starting at the position of the first unescaped colon and the
 * first unescaped "at" sign. This is normally used to unescape
 * device names used as PVs.
 */
void dm_unescape_colons_and_at_signs(char *src,
				     char **substr_first_unquoted_colon,
				     char **substr_first_unquoted_at_sign)
{
	const char *orig_chars = ":@";
	char *arr_substr_first_unquoted[] = {NULL, NULL, NULL};

	_unquote_characters(src, orig_chars, 2, '\\', arr_substr_first_unquoted);

	if (substr_first_unquoted_colon)
		*substr_first_unquoted_colon = arr_substr_first_unquoted[0];

	if (substr_first_unquoted_at_sign)
		*substr_first_unquoted_at_sign = arr_substr_first_unquoted[1];
}

