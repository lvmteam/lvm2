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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/misc/lvm-string.h"
#include "lib/metadata/metadata-exported.h"
#include "lib/display/display.h"

#include <ctype.h>
#include <stdarg.h>

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

static name_error_t _validate_name(const char *n)
{
	register char c;
	register int len = 0;

	if (!n || !*n)
		return NAME_INVALID_EMPTY;

	/* Hyphen used as VG-LV separator - ambiguity if LV starts with it */
	if (*n == '-')
		return NAME_INVALID_HYPHEN;

	if ((*n == '.') && (!n[1] || (n[1] == '.' && !n[2]))) /* ".", ".." */
		return NAME_INVALID_DOTS;

	while ((len++, c = *n++))
		if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+')
			return NAME_INVALID_CHARSET;

	if (len > NAME_LEN)
		return NAME_INVALID_LENGTH;

	return NAME_VALID;
}

/*
 * Device layer names are all of the form <vg>-<lv>-<layer>, any
 * other hyphens that appear in these names are quoted with yet
 * another hyphen.  The top layer of any device has no layer
 * name.  eg, vg0-lvol0.
 */
int validate_name(const char *n)
{
	return (_validate_name(n) == NAME_VALID) ? 1 : 0;
}

/*
 * Copy valid systemid characters from source to destination.
 * Invalid characters are skipped.  Copying is stopped
 * when NAME_LEN characters have been copied.
 * A terminating NUL is appended.
 */
void copy_systemid_chars(const char *src, char *dst)
{
	const char *s = src;
	char *d = dst;
	int len = 0;
	char c;

	if (!s || !*s)
		return;

	/* Skip non-alphanumeric starting characters */
	while (*s && !isalnum(*s))
		s++;

	while ((c = *s++)) {
		if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+')
			continue;

		*d++ = c;

		if (++len >= NAME_LEN)
			break;
	}

	*d = '\0';
}

static const char *_lvname_has_reserved_prefix(const char *lvname)
{
	static const char _prefixes[][12] = {
		"pvmove",
		"snapshot"
	};
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(_prefixes); ++i)
		if (!strncmp(lvname, _prefixes[i], strlen(_prefixes[i])))
			return _prefixes[i];

	return NULL;
}

static const char *_lvname_has_reserved_component_string(const char *lvname)
{
	static const char _strings[][12] = {
		/* Suffixes for component LVs */
		"_cdata",
		"_cmeta",
		"_corig",
		"_cpool",
		"_cvol",
		"_wcorig",
		"_mimage",
		"_mlog",
		"_rimage",
		"_rmeta",
		"_tdata",
		"_tmeta",
		"_vdata",
		"_imeta",
		"_iorig"
	};
	unsigned i;

	if (!(lvname = strchr(lvname, '_')))
		return NULL;

	for (i = 0; i < DM_ARRAY_SIZE(_strings); ++i)
		if (strstr(lvname, _strings[i]))
			return _strings[i];

	return NULL;
}

static const char *_lvname_has_reserved_string(const char *lvname)
{
	static const char _strings[][12] = {
		/* Additional suffixes for non-component LVs */
		"_pmspare",
		"_vorigin"
	};
	unsigned i;
	const char *cs;

	if (!(lvname = strchr(lvname, '_')))
		return NULL;

	if ((cs = _lvname_has_reserved_component_string(lvname)))
		return cs;

	for (i = 0; i < DM_ARRAY_SIZE(_strings); ++i)
		if (strstr(lvname, _strings[i]))
			return _strings[i];

	return NULL;
}


int apply_lvname_restrictions(const char *name)
{
	const char *s;

	if ((s = _lvname_has_reserved_prefix(name))) {
		log_error("Names starting \"%s\" are reserved. "
			  "Please choose a different LV name.", s);
		return 0;
	}

	if ((s = _lvname_has_reserved_string(name))) {
		log_error("Names including \"%s\" are reserved. "
			  "Please choose a different LV name.", s);
		return 0;
	}

	return 1;
}

/*
 * Validates name and returns an enumerated reason for name validation failure.
 */
name_error_t validate_name_detailed(const char *name)
{
	return _validate_name(name);
}

int is_reserved_lvname(const char *name)
{
	return (_lvname_has_reserved_prefix(name) ||
		_lvname_has_reserved_string(name)) ? 1 : 0;
}

int is_component_lvname(const char *name)
{
	return (_lvname_has_reserved_component_string(name)) ? 1 : 0;
}

char *build_dm_uuid(struct dm_pool *mem, const struct logical_volume *lv,
		    const char *layer)
{
	const char *lvid = lv->lvid.s;
	char *dlid;

	if (!layer) {
		/*
		 * Mark internal LVs with layer suffix
		 * so tools like blkid may immediately see it's
		 * an internal LV they should not scan.
		 * Should also make internal detection simpler.
		 */
		/* Suffixes used here MUST match lib/activate/dev_manager.c */
		layer = lv_is_cache_origin(lv) ? "real" :
			lv_is_writecache_origin(lv) ? "real" :
			(lv_is_cache(lv) && lv_is_pending_delete(lv)) ? "real" :
			lv_is_cache_pool_data(lv) ? "cdata" :
			lv_is_cache_pool_metadata(lv) ? "cmeta" :
			lv_is_cache_vol(lv) ? "cvol" :
			((lv_is_mirror_image(lv) ||
			  lv_is_mirror_log(lv) ||
			  lv_is_integrity_origin(lv) ||
			  lv_is_integrity_metadata(lv) ||
			  lv_is_raid_image(lv) ||
			  lv_is_raid_metadata(lv)) &&
			 !lv_is_visible(lv)) ? "real" :
			lv_is_pvmove(lv) ? "real" :
			lv_is_thin_pool(lv) ? "pool" :
			lv_is_thin_pool_data(lv) ? "tdata" :
			lv_is_thin_pool_metadata(lv) ? "tmeta" :
			lv_is_vdo_pool(lv) ? "pool" :
			lv_is_vdo_pool_data(lv) ? "vdata" :
			NULL;
	}

	/* Temporary mirror layer can be only recognized by checking its name.
	 * LV appears as public LV for initial activation. */
	if (!layer && strstr(lv->name, MIRROR_SYNC_LAYER "_"))
		layer = "real";

	if (!(dlid = dm_build_dm_uuid(mem, UUID_PREFIX, lvid, layer)))
		log_error("Failed to build LVM dlid for %s.",
			  display_lvname(lv));

	return dlid;
}

char *first_substring(const char *str, ...)
{
	char *substr, *r = NULL;
	va_list ap;

	va_start(ap, str);

	while ((substr = va_arg(ap, char *)))
		if ((r = strstr(str, substr)))
			break;

	va_end(ap);

	return r;
}

/* Cut suffix (if present) and write the name into NAME_LEN sized new_name buffer
 * When suffix is NULL, everything past the last '_' is removed.
 * Returns 1 when suffix was removed, 0 otherwise.
 */
int drop_lvname_suffix(char *new_name, const char *name, const char *suffix)
{
	char *c;

	if (!_dm_strncpy(new_name, name, NAME_LEN)) {
		log_debug(INTERNAL_ERROR "Name is too long.");
		return 0;
	}

	if (!(c = strrchr(new_name, '_')))
		return 0;

	if (suffix && strcmp(c + 1, suffix))
		return 0;

	*c = 0; /* remove suffix */

	return 1;
}

void split_line(char *buf, int *argc, char **argv, int max_args, char sep)
{
	char *p = buf;
	int i;

	argv[0] = p;

	for (i = 1; i < max_args; i++) {
		p = strchr(p, sep);
		if (!p)
			break;
		*p++ = '\0';

		argv[i] = p;
	}
	*argc = i;
}
