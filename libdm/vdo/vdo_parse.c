/*
 * Copyright (C) 2018-2026 Red Hat, Inc. All rights reserved.
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
 * Shared parsing helpers for VDO status and stats parsers.
 */

#include "vdo/vdo_parse.h"

#include <ctype.h>

const char *vdo_parse_eat_space(const char *b, const char *e)
{
	while (b != e && isspace((unsigned char)*b))
		b++;

	return b;
}

int vdo_parse_tok_eq(const char *b, const char *e, const char *str)
{
	while (b != e) {
		if (!*str || *b != *str)
			return 0;

		b++;
		str++;
	}

	return !*str;
}

int vdo_parse_uint64(const char *b, const char *e, void *context)
{
	uint64_t *r = context, n;

	n = 0;
	while (b != e) {
		if (!isdigit((unsigned char)*b))
			return 0;

		n = (n * 10) + (*b - '0');
		b++;
	}

	*r = n;
	return 1;
}

int vdo_parse_operating_mode(const char *b, const char *e, void *context)
{
	static const struct {
		const char str[12];
		enum dm_vdo_operating_mode mode;
	} _table[] = {
		{ "recovering", DM_VDO_MODE_RECOVERING },
		{ "read-only", DM_VDO_MODE_READ_ONLY },
		{ "normal", DM_VDO_MODE_NORMAL },
	};

	enum dm_vdo_operating_mode *r = context;
	unsigned i;
	for (i = 0; i < DM_ARRAY_SIZE(_table); i++) {
		if (vdo_parse_tok_eq(b, e, _table[i].str)) {
			*r = _table[i].mode;
			return 1;
		}
	}

	return 0;
}
