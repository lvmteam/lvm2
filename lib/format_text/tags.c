/*
 * Copyright (C) 2003 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "metadata.h"
#include "import-export.h"
#include "pool.h"
#include "str_list.h"
#include "lvm-string.h"

int print_tags(struct list *tags, char *buffer, size_t size)
{
	struct str_list *sl;
	int first = 1;

	if (!emit_to_buffer(&buffer, &size, "[")) {
		stack;
		return 0;
	}

	list_iterate_items(sl, tags) {
		if (!first) {
			if (!emit_to_buffer(&buffer, &size, ", ")) {
				stack;
				return 0;
			}
		} else
			first = 0;

		if (!emit_to_buffer(&buffer, &size, "\"%s\"", sl->str)) {
			stack;
			return 0;
		}
	}

	if (!emit_to_buffer(&buffer, &size, "]")) {
		stack;
		return 0;
	}

	return 1;
}

int read_tags(struct pool *mem, struct list *tags, struct config_value *cv)
{
	if (cv->type == CFG_EMPTY_ARRAY)
		return 1;

	while (cv) {
		if (cv->type != CFG_STRING) {
			log_error("Found a tag that is not a string");
			return 0;
		}

		if (!str_list_add(mem, tags, pool_strdup(mem, cv->v.str))) {
			stack;
			return 0;
		}

		cv = cv->next;
	}

	return 1;
}
