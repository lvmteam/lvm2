/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "lvm-string.h"

#include <ctype.h>

int emit_to_buffer(char **buffer, size_t *size, const char *fmt, ...)
{
	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(*buffer, *size, fmt, ap);
	va_end(ap);

	/*
	 * Revert to old glibc behaviour (version <= 2.0.6) where snprintf
	 * returned -1 if buffer was too small. From glibc 2.1 it returns number
	 * of chars that would have been written had there been room.
	 */
	if (n < 0 || ((unsigned) n + 1 > *size))
		n = -1;

	if (n < 0 || ((size_t)n == *size))
		return 0;

	*buffer += n;
	*size -= n;
	return 1;
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
 * Count occurences of 'c' in 'str' of length 'size'.
 *
 * Returns:
 *   Number of occurrences of 'c'
 */
unsigned count_chars(const char *str, size_t len, const int c)
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
size_t escaped_len(const char *str)
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
char *build_dm_name(struct dm_pool *mem, const char *vgname,
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

char *build_dm_uuid(struct dm_pool *mem, const char *lvid, const char *layer)
{
	char *dmuuid;
	size_t len;

	if (!layer)
		layer = "";

	len = sizeof(UUID_PREFIX) + strlen(lvid) + strlen(layer) + 1;

	if (!(dmuuid = dm_pool_alloc(mem, len))) {
		log_error("build_dm_name: Allocation failed for %" PRIsize_t
			  " %s %s.", len, lvid, layer);
		return NULL;
	}

	sprintf(dmuuid, UUID_PREFIX "%s%s%s", lvid, (*layer) ? "-" : "", layer);

	return dmuuid;
}

/*
 * Copies a string, quoting double quotes with backslashes.
 */
char *escape_double_quotes(char *out, const char *src)
{
	char *buf = out;

	_quote_characters(&buf, src, '\"', '\\', 1);
	*buf = '\0';

	return out;
}

/*
 * Undo quoting in situ.
 */
void unescape_double_quotes(char *src)
{
	_unquote_one_character(src, '\"', '\\');
}

/*
 * Unescape colons and "at" signs in situ and save the substrings
 * starting at the position of the first unescaped colon and the
 * first unescaped "at" sign. This is normally used to unescape
 * device names used as PVs.
 */
void unescape_colons_and_at_signs(char *src,
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

/*
 * A-Za-z0-9._-+/=!:&#
 */
int validate_tag(const char *n)
{
	register char c;
	register int len = 0;

	if (!n || !*n)
		return 0;

	while ((len++, c = *n++))
		if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+' && c != '/'
		    && c != '=' && c != '!' && c != ':' && c != '&' && c != '#')
			return 0;

	return 1;
}

/*
 * Device layer names are all of the form <vg>-<lv>-<layer>, any
 * other hyphens that appear in these names are quoted with yet
 * another hyphen.  The top layer of any device has no layer
 * name.  eg, vg0-lvol0.
 */
int validate_name(const char *n)
{
	register char c;
	register int len = 0;

	if (!n || !*n)
		return 0;

	/* Hyphen used as VG-LV separator - ambiguity if LV starts with it */
	if (*n == '-')
		return 0;

	if (!strcmp(n, ".") || !strcmp(n, ".."))
		return 0;

	while ((len++, c = *n++))
		if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+')
			return 0;

	if (len > NAME_LEN)
		return 0;

	return 1;
}

int apply_lvname_restrictions(const char *name)
{
	if (!strncmp(name, "snapshot", 8)) {
		log_error("Names starting \"snapshot\" are reserved. "
			  "Please choose a different LV name.");
		return 0;
	}

	if (!strncmp(name, "pvmove", 6)) {
		log_error("Names starting \"pvmove\" are reserved. "
			  "Please choose a different LV name.");
		return 0;
	}

	if (strstr(name, "_mlog")) {
		log_error("Names including \"_mlog\" are reserved. "
			  "Please choose a different LV name.");
		return 0;
	}

	if (strstr(name, "_mimage")) {
		log_error("Names including \"_mimage\" are reserved. "
			  "Please choose a different LV name.");
		return 0;
	}

	if (strstr(name, "_rimage")) {
		log_error("Names including \"_rimage\" are reserved. "
			  "Please choose a different LV name.");
		return 0;
	}

	if (strstr(name, "_rmeta")) {
		log_error("Names including \"_rmeta\" are reserved. "
			  "Please choose a different LV name.");
		return 0;
	}

	if (strstr(name, "_vorigin")) {
		log_error("Names including \"_vorigin\" are reserved. "
			  "Please choose a different LV name.");
		return 0;
	}

	return 1;
}

int is_reserved_lvname(const char *name)
{
	int rc, old_suppress;

	old_suppress = log_suppress(2);
	rc = !apply_lvname_restrictions(name);
	log_suppress(old_suppress);

	return rc;
}
