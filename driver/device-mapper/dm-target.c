/*
 * dm-target.c
 *
 * Copyright (C) 2001 Sistina Software (UK) Limited
 *
 * This file is released under the GPL.
 */

/*
 * 16/08/2001 - First Version [Joe Thornber]
 */

#include "dm.h"
#include <linux/kmod.h>

static LIST_HEAD(_targets);
static rwlock_t _lock = RW_LOCK_UNLOCKED;

#define DM_MOD_NAME_SIZE 32

static inline struct target_type *__get_target_type(const char *name)
{
	struct list_head *tmp, *head;
	struct target_type *t;

	for(tmp = _targets.next; tmp != &_targets; tmp = tmp->next) {

		t = list_entry(tmp, struct target_type, list);
		if (!strcmp(name, t->name)) {
			if (!t->use && t->module)
				__MOD_INC_USE_COUNT(t->module);
			t->use++;
			return t;
		}
	}

	return 0;
}

static struct target_type *get_target_type(const char *name)
{
	read_lock(&_lock);
	t = __get_target_type(name);
	read_unlock(&_lock);

	return t;
}

static void load_module(const char *name)
{
	char module_name[DM_MOD_NAME_SIZE] = "dm-";

	/* Length check for strcat() below */
	if (strlen(name) > (DM_MOD_NAME_SIZE - 4))
		return NULL;

	/* strcat() is only safe due to length check above */
	strcat(module_name, name);
	request_module(module_name);
}

struct target_type *dm_get_target_type(const char *name)
{
	t = get_target_type(name);

	if (!t) {
		load_module(name);
		t = get_target_type(name);
	}

	return t;
}

void dm_put_target_type(struct target_type *t)
{
	read_lock(&_lock);
	if (--t->use == 0 && t->module)
		__MOD_DEC_USE_COUNT(t->module);

	if (t->use < 0)
		BUG();
	read_unlock(&_lock);
}

int dm_register_target(struct target_type *t)
{
	int rv = 0;

	write_lock(&_lock);
	if (__get_target_type(t->name)) {
		rv = -EEXIST;
		goto out;
	}
	list_add(&t->list, &_targets);

 out:
	write_unlock(&_lock);
	return rv;
}

int dm_unregister_target(struct target_type *t)
{
	int rv = -ETXTBSY;

	write_lock(&_lock);
	if (t->use == 0) {
		list_del(&t->list);
		rv = 0;
	}
	write_unlock(&_lock);

	return rv;
}

/*
 * io-err: always fails an io, useful for bringing
 * up LV's that have holes in them.
 */
static void *io_err_ctr(struct dm_table *t, offset_t b, offset_t l,
		      struct text_region *args)
{
	/* this takes no arguments */
	return NULL;
}

static void io_err_dtr(struct dm_table *t, void *c)
{
	/* empty */
}

static int io_err_map(struct buffer_head *bh, int rw, void *context)
{
	buffer_IO_error(bh);
	return 0;
}

static struct target_type error_target = {
	name: "error",
	ctr: io_err_ctr,
	dtr: io_err_dtr,
	map: io_err_map
};


int dm_target_init(void)
{
	return dm_register_target(&error_target);
}

EXPORT_SYMBOL(dm_register_target);
EXPORT_SYMBOL(dm_unregister_target);

