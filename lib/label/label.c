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
		log_error("Couldn't allocate memory for labeller list object.");
		return NULL;
	}

	li->l = l;
	strcpy(li->name, name);

	return li;
}

static void _free_li(struct labeller_i *li)
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
	struct labeller_i *li;

	for (c = _labellers.n; c != &_labellers; c = n) {
		n = c->n;
		li = list_item(c, struct labeller_i);
		_free_li(li);
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

static struct labeller *_find_labeller(struct device *dev)
{
	struct list *lih;
	struct labeller_i *li;

	list_iterate (lih, &_labellers) {
		li = list_item(lih, struct labeller_i);
		if (li->l->ops->can_handle(li->l, dev))
			return li->l;
	}

	log_debug("No label on device '%s'.", dev_name(dev));
	return NULL;
}

int label_remove(struct device *dev)
{
	struct labeller *l;

	if (!(l = _find_labeller(dev))) {
		stack;
		return 0;
	}

	return l->ops->remove(l, dev);
}

int label_read(struct device *dev, struct label **result)
{
	struct labeller *l;
	int r;

	if (!(l = _find_labeller(dev))) {
		stack;
		return 0;
	}

	if ((r = l->ops->read(l, dev, result)))
		(*result)->labeller = l;

	return r;
}

int label_verify(struct device *dev)
{
	struct labeller *l;

	if (!(l = _find_labeller(dev))) {
		stack;
		return 0;
	}

	return l->ops->verify(l, dev);
}

void label_destroy(struct label *lab)
{
	lab->labeller->ops->destroy_label(lab->labeller, lab);
}
