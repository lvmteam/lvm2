/*
 * Copyright (C) 2022-2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Parser for VDO kernel stats message response string.
 * Kernel emits stats as nested key-value pairs:
 *   { key : value, key : { nested_key : value, }, ... }
 *
 * Exports:
 *   dm_vdo_stats_parse() — fill dm_vdo_stats, optionally with label/value fields
 */

#include "libdm/misc/dmlib.h"
#include "libdm/libdevmapper.h"
#include "vdo/vdo_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STAT_LENGTH 80
#define MAX_STATS 250

static const char *_next_token(const char *b, const char *e)
{
	const char *te = b;
	while (te != e && !isspace((unsigned char)*te) && *te != ':' &&
	       *te != ',' && *te != '{' && *te != '}')
		te++;
	return te == b ? NULL : te;
}

/*----------------------------------------------------------------
 * Build label by converting camel case to spaced
 *----------------------------------------------------------------*/

static void _build_label(const char *prefix,
			 const char *kb,
			 const char *ke,
			 char *dst,
			 size_t dst_len)
{
	const char *ks = kb;
	size_t j;

	for (j = 0; *prefix && j < dst_len - 1; prefix++)
		dst[j++] = *prefix;

	if (j > 0 && j < dst_len - 1)
		dst[j++] = ' ';

	for (; kb < ke && j < dst_len - 1; kb++) {
		unsigned char c = *kb;
		unsigned char prev = (kb > ks) ? *(kb - 1) : 0;
		unsigned char next = (kb + 1 < ke) ? *(kb + 1) : 0;

		if (kb > ks && isupper(c)) {
			if (islower(prev)) {
				if (j < dst_len - 2)
					dst[j++] = ' ';
				dst[j++] = islower(next) ? tolower(c) : c;
			} else if (isupper(prev) && islower(next)) {
				unsigned char next2 =
					(kb + 2 < ke) ? *(kb + 2) : 0;
				if (!next2 || isupper(next2)) {
					dst[j++] = c;
				} else {
					if (j < dst_len - 2)
						dst[j++] = ' ';
					dst[j++] = tolower(c);
				}
			} else {
				dst[j++] = c;
			}
		} else {
			dst[j++] = c;
		}
	}
	dst[j] = '\0';
}

/*----------------------------------------------------------------
 * Store a parsed label/value pair.
 *
 * Always fills dm_vdo_stats struct fields.  When field_count < MAX_STATS
 * (full mode), also records each label/value into fields[].
 *----------------------------------------------------------------*/

static void _parse_field(const char *label, const char *val,
		      struct dm_vdo_stats_full *f)
{
	struct dm_vdo_stats *s = f->stats;
	const char *ve = val + strlen(val);

	if (!strcmp(label, "data blocks used"))
		vdo_parse_uint64(val, ve, &s->data_blocks_used);
	else if (!strcmp(label, "overhead blocks used"))
		vdo_parse_uint64(val, ve, &s->overhead_blocks_used);
	else if (!strcmp(label, "logical blocks used"))
		vdo_parse_uint64(val, ve, &s->logical_blocks_used);
	else if (!strcmp(label, "physical blocks"))
		vdo_parse_uint64(val, ve, &s->physical_blocks);
	else if (!strcmp(label, "logical blocks"))
		vdo_parse_uint64(val, ve, &s->logical_blocks);
	else if (!strcmp(label, "block size"))
		vdo_parse_uint64(val, ve, &s->bytes_per_physical_block);
	else if (!strcmp(label, "logical block size"))
		vdo_parse_uint64(val, ve, &s->bytes_per_logical_block);
	else if (!strcmp(label, "bios in write"))
		vdo_parse_uint64(val, ve, &s->bios_in);
	else if (!strcmp(label, "bios out write"))
		vdo_parse_uint64(val, ve, &s->bios_out);
	else if (!strcmp(label, "bios meta write"))
		vdo_parse_uint64(val, ve, &s->bios_meta);
	else if (!strcmp(label, "mode")) {
		if (!vdo_parse_operating_mode(val, ve, &s->operating_mode))
			s->operating_mode = DM_VDO_MODE_NORMAL;
	}

	if (f->field_count < MAX_STATS) {
		struct dm_vdo_stats_field *fld = &f->fields[f->field_count];
		snprintf(fld->label, sizeof(fld->label), "%s", label);
		snprintf(fld->value, sizeof(fld->value), "%s", val);
		f->field_count++;
	}
}

/*----------------------------------------------------------------
 * Internal stats walker
 *
 * Single traversal of the nested { key : value } string.
 * Builds hierarchical labels (e.g. "bios in write") and calls
 * _parse_field for each leaf value encountered.
 *----------------------------------------------------------------*/

#define MAX_NESTING_DEPTH 16

static const char *_parse_group(const char *b,
				const char *e,
				const char *prefix,
				struct dm_vdo_stats_full *f,
				int depth)
{
	const char *ke, *ve;

	if (depth > MAX_NESTING_DEPTH)
		return e;

	while (b < e && *b != '}') {
		char label[MAX_STAT_LENGTH];
		char vbuf[MAX_STAT_LENGTH];
		size_t vlen;

		while (b < e && *b == ',')
			b = vdo_parse_eat_space(b + 1, e);
		if (b >= e || *b == '}')
			break;

		ke = _next_token(b, e);
		if (!ke)
			break;

		_build_label(prefix, b, ke, label, sizeof(label));

		b = vdo_parse_eat_space(ke, e);
		if (b >= e || *b != ':')
			break;
		b = vdo_parse_eat_space(b + 1, e);

		if (b < e && *b == '{') {
			b = _parse_group(vdo_parse_eat_space(b + 1, e), e,
					 label, f, depth + 1);
			if (b < e && *b == '}')
				b = vdo_parse_eat_space(b + 1, e);
		} else {
			ve = _next_token(b, e);
			if (!ve)
				break;

			vlen = (size_t) (ve - b);
			if (vlen >= sizeof(vbuf))
				vlen = sizeof(vbuf) - 1;
			memcpy(vbuf, b, vlen);
			vbuf[vlen] = '\0';

			_parse_field(label, vbuf, f);
			b = vdo_parse_eat_space(ve, e);
		}
	}

	return b;
}

/*----------------------------------------------------------------
 * dm_vdo_stats_parse
 *
 * Returns a single contiguous allocation containing the
 * dm_vdo_stats_full header, the dm_vdo_stats, and (when
 * DM_VDO_STATS_FULL is set) the label/value fields[].
 * Caller frees with dm_free() (or lets the pool handle it).
 *----------------------------------------------------------------*/

struct dm_vdo_stats_full *dm_vdo_stats_parse(struct dm_pool *mem,
					     const char *stats_str,
					     unsigned flags)
{
	struct dm_vdo_stats_full *f;
	size_t fields_sz;
	size_t alloc_sz;
	char *base;
	const char *b, *e;

	if (!stats_str)
		return NULL;

	fields_sz = (flags & DM_VDO_STATS_FULL)
		? (size_t) MAX_STATS * sizeof(struct dm_vdo_stats_field)
		: 0;

	alloc_sz = sizeof(struct dm_vdo_stats_full) +
		   fields_sz +
		   sizeof(struct dm_vdo_stats);

	base = (!mem) ? dm_zalloc(alloc_sz) : dm_pool_zalloc(mem, alloc_sz);
	if (!base)
		return NULL;

	f = (struct dm_vdo_stats_full *) base;
	f->stats = (struct dm_vdo_stats *)(base + sizeof(*f) + fields_sz);
	f->field_count = (flags & DM_VDO_STATS_FULL) ? 0 : (MAX_STATS + 1);

	e = stats_str + strlen(stats_str);
	b = vdo_parse_eat_space(stats_str, e);

	if (b < e && *b == '{')
		b = vdo_parse_eat_space(b + 1, e);

	_parse_group(b, e, "", f, 0);

	if (f->field_count > MAX_STATS)
		f->field_count = 0;

	return f;
}
