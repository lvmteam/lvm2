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
#include "lvm-string.h"

#include <ctype.h>

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
char *build_dm_name(struct dm_pool *mem, const char *vgname,
		    const char *lvname, const char *layer)
{
	size_t len = 1;
	int hyphens = 1;
	char *r, *out;

	_count_hyphens(vgname, &len, &hyphens);
	_count_hyphens(lvname, &len, &hyphens);

	if (layer && *layer) {
		_count_hyphens(layer, &len, &hyphens);
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
