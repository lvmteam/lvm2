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
 * A-Za-z0-9._-+/=!:&#
 */
int validate_tag(const char *n)
{
	register char c;
	/* int len = 0; */

	if (!n || !*n)
		return 0;

	/* FIXME: Is unlimited tag size support needed ? */
	while ((/* len++, */ c = *n++))
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

	if ((*n == '.') && (!n[1] || (n[1] == '.' && !n[2]))) /* ".", ".." */
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
	static const char * const _reserved_prefixes[] = {
		"snapshot",
		"pvmove",
		NULL
	};

	static const char * const _reserved_strings[] = {
		"_mlog",
		"_mimage",
		"_pmspare",
		"_rimage",
		"_rmeta",
		"_vorigin",
		"_tdata",
		"_tmeta",
		NULL
	};

	unsigned i;
	const char *s;

	for (i = 0; (s = _reserved_prefixes[i]); i++) {
		if (!strncmp(name, s, strlen(s))) {
			log_error("Names starting \"%s\" are reserved. "
				  "Please choose a different LV name.", s);
			return 0;
		}
	}

	for (i = 0; (s = _reserved_strings[i]); i++) {
		if (strstr(name, s)) {
			log_error("Names including \"%s\" are reserved. "
				  "Please choose a different LV name.", s);
			return 0;
		}
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

char *build_dm_uuid(struct dm_pool *mem, const char *lvid,
		    const char *layer)
{
	return dm_build_dm_uuid(mem, UUID_PREFIX, lvid, layer);
}
