/*
 * Copyright (C) 2002 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "label.h"
#include "list.h"
#include "dbg_malloc.h"
#include "log.h"

/*
 * Internal labeller struct.
 */
struct labeller_i {
	struct list list;

	struct labeller *l;
	char name[0];
};

static struct list _labellers;


static struct labeller_i *_alloc_li(const char *name, struct labeller *l)
{
	struct labeller_i *li;
	size_t len;

	len = sizeof(*li) + strlen(name) + 1;

	if (!(li = dbg_malloc(len))) {
		log_err("Couldn't allocate memory for labeller list object.");
		return NULL;
	}

	li->l = l;
	strcpy(li->name, name);

	return li;
}

static void _free_li(struct list *li)
{
	dbg_free(li);
}


int label_init(void)
{
	list_init(&_labellers);
	return 1;
}

void label_exit(void)
{
	struct list *c, *n;
	struct labeller_list *ll;

	for (c = _labellers.n; c != &_labellers; c = n) {
		n = c->n;
		ll = list_item(c, struct labeller_list);
		_free_li(c);
	}
}

int label_register_handler(const char *name, struct labeller *handler)
{
	struct labeller_i *li;

	if (!(li = _alloc_li(name, handler))) {
		stack;
		return 0;
	}

	list_add(&_labellers, &li->list);
	return 1;
}

struct labeller *label_get_handler(const char *name)
{
	struct list *lih;
	struct labeller_i *li;

	list_iterate (lih, &_labellers) {
		li = list_item(lih, struct labeller_i);
		if (!strcmp(li->name, name))
			return li->l;
	}

	return NULL;
}

static struct labeller *_find_labeller(const char *device)
{
	struct list *lih;
	struct labeller_i *li;

	list_iterate (lih, &_labellers) {
		li = list_item(lih, struct labeller_i);
		if (li->l->ops->can_handle(device))
			return li->l;
	}

	log_err("Could not find label on device '%s'.", device);
	return NULL;
}

int label_remove(const char *device)
{
	struct labeller *l;

	if (!(l = _find_labeller(device))) {
		stack;
		return 0;
	}

	return l->ops->remove(device);
}

int label_read(const char *path, struct label **result)
{
	struct labeller *l;

	if (!(l = _find_labeller(device))) {
		stack;
		return 0;
	}

	return l->ops->read(device, label);
}

int label_verify(const char *path)
{
	struct labeller *l;

	if (!(l = _find_labeller(device))) {
		stack;
		return 0;
	}

	return l->ops->verify(device);
}

void label_free(struct label *l)
{
	dbg_free(l->extra_info);
	dbg_free(l);
}

