/*
 * Copyright (C) 2003 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "str_list.h"

int str_list_add(struct pool *mem, struct list *sl, const char *str)
{
	struct str_list *sln;
	struct list *slh;

	if (!str) {
		stack;
		return 0;
	}

	/* Already in list? */
	list_iterate(slh, sl) {
		if (!strcmp(str, list_item(slh, struct str_list)->str))
			 return 1;
	}

	if (!(sln = pool_alloc(mem, sizeof(*sln)))) {
		stack;
		return 0;
	}

	sln->str = str;
	list_add(sl, &sln->list);

	return 1;
}

int str_list_del(struct list *sl, const char *str)
{
	struct list *slh, *slht;

	list_iterate_safe(slh, slht, sl) {
		if (!strcmp(str, list_item(slh, struct str_list)->str))
			 list_del(slh);
	}

	return 1;
}

