/*
 * Copyright (C) 2003 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "str_list.h"

struct list *str_list_create(struct pool *mem)
{
	struct list *sl;

	if (!(sl = pool_alloc(mem, sizeof(struct list)))) {
		stack;
		return NULL;
	}

	list_init(sl);

	return sl;
}

int str_list_add(struct pool *mem, struct list *sll, const char *str)
{
	struct str_list *sln;

	if (!str) {
		stack;
		return 0;
	}

	/* Already in list? */
	if (str_list_match_item(sll, str))
		return 1;

	if (!(sln = pool_alloc(mem, sizeof(*sln)))) {
		stack;
		return 0;
	}

	sln->str = str;
	list_add(sll, &sln->list);

	return 1;
}

int str_list_del(struct list *sll, const char *str)
{
	struct list *slh, *slht;

	list_iterate_safe(slh, slht, sll) {
		if (!strcmp(str, list_item(slh, struct str_list)->str))
			 list_del(slh);
	}

	return 1;
}

int str_list_dup(struct pool *mem, struct list *sllnew, struct list *sllold)
{
	struct str_list *sl;

	list_init(sllnew);

	list_iterate_items(sl, sllold) {
		if (!str_list_add(mem, sllnew, strdup(sl->str))) {
			stack;
			return 0;
		}
	}

	return 1;
}

/*
 * Is item on list?
 */
int str_list_match_item(struct list *sll, const char *str)
{
	struct str_list *sl;

	list_iterate_items(sl, sll)
	    if (!strcmp(str, sl->str))
		return 1;

	return 0;
}

/*
 * Is at least one item on both lists?
 */
int str_list_match_list(struct list *sll, struct list *sll2)
{
	struct str_list *sl;

	list_iterate_items(sl, sll)
	    if (str_list_match_item(sll2, sl->str))
		return 1;

	return 0;
}

/*
 * Do both lists contain the same set of items?
 */
int str_list_lists_equal(struct list *sll, struct list *sll2)
{
	struct str_list *sl;

	if (list_size(sll) != list_size(sll2))
		return 0;

	list_iterate_items(sl, sll)
	    if (!str_list_match_item(sll2, sl->str))
		return 0;

	return 1;
}
