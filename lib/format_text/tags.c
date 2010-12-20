/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
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
#include "metadata.h"
#include "import-export.h"
#include "str_list.h"
#include "lvm-string.h"

char *alloc_printed_tags(struct dm_list *tags)
{
	struct str_list *sl;
	int first = 1;
	size_t size = 0;
	char *buffer, *buf;

	dm_list_iterate_items(sl, tags)
		/* '"' + tag + '"' + ',' + ' ' */
		size += strlen(sl->str) + 4;
	/* '[' + ']' + '\0' */
	size += 3;

	if (!(buffer = buf = dm_malloc(size))) {
		log_error("Could not allocate memory for tag list buffer.");
		return NULL;
	}

	if (!emit_to_buffer(&buf, &size, "["))
		goto_bad;

	dm_list_iterate_items(sl, tags) {
		if (!first) {
			if (!emit_to_buffer(&buf, &size, ", "))
				goto_bad;
		} else
			first = 0;

		if (!emit_to_buffer(&buf, &size, "\"%s\"", sl->str))
			goto_bad;
	}

	if (!emit_to_buffer(&buf, &size, "]"))
		goto_bad;

	return buffer;

bad:
	dm_free(buffer);
	return_NULL;
}

int read_tags(struct dm_pool *mem, struct dm_list *tags, const struct config_value *cv)
{
	if (cv->type == CFG_EMPTY_ARRAY)
		return 1;

	while (cv) {
		if (cv->type != CFG_STRING) {
			log_error("Found a tag that is not a string");
			return 0;
		}

		if (!str_list_add(mem, tags, dm_pool_strdup(mem, cv->v.str)))
			return_0;

		cv = cv->next;
	}

	return 1;
}
