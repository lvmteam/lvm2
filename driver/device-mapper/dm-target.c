/*
 * dm-target.c
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This software is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * 16/08/2001 - First Version [Joe Thornber]
 */

#include "dm.h"
#include <linux/kmod.h>

static LIST_HEAD(dm_targets);
static rwlock_t dm_targets_lock = RW_LOCK_UNLOCKED;

#define DM_MOD_NAME_SIZE 32

struct target_type *dm_get_target_type(const char *name)
{
	struct list_head *tmp, *head;
	struct target_type *t;
	int try = 0;

	/* Length check for strcat() below */
	if (strlen(name) > (DM_MOD_NAME_SIZE - 4))
		return NULL;

try_again:
	read_lock(&dm_targets_lock);
	tmp = head = &dm_targets;
	for(;;) {
		tmp = tmp->next;
		if (tmp == head)
			break;
		t = list_entry(tmp, struct target_type, list);
		if (strcmp(name, t->name) == 0) {
			if (t->use == 0 && t->module)
				__MOD_INC_USE_COUNT(t->module);
			t->use++;
			read_unlock(&dm_targets_lock);
			return t;
		}
	}
	read_unlock(&dm_targets_lock);

	if (try++ == 0) {
		char module_name[DM_MOD_NAME_SIZE] = "dm-";
		/* strcat() is only safe due to length check above */
		strcat(module_name, name);
		request_module(module_name);
		goto try_again;
	}

	return NULL;
}

void dm_put_target_type(struct target_type *t)
{
	read_lock(&dm_targets_lock);
	if (--t->use == 0 && t->module)
		__MOD_DEC_USE_COUNT(t->module);
	if (t->use < 0)
		BUG();
	read_unlock(&dm_targets_lock);
}

int dm_register_target(struct target_type *t)
{
	struct list_head *tmp, *head;
	struct target_type *t2;
	int rv = 0;
	write_lock(&dm_targets_lock);
	tmp = head = &dm_targets;
	for(;;) {
		if (tmp == head)
			break;
		t2 = list_entry(tmp, struct target_type, list);
		if (strcmp(t->name, t2->name) != 0)
			continue;
		rv = -EEXIST;
		break;
	}
	if (rv == 0)
		list_add(&t->list, &dm_targets);
	write_unlock(&dm_targets_lock);
	return rv;
}

int dm_unregister_target(struct target_type *t)
{
	int rv = -ETXTBSY;

	write_lock(&dm_targets_lock);
	if (t->use == 0) {
		list_del(&t->list);
		rv = 0;
	}
	write_unlock(&dm_targets_lock);

	return rv;
}

/*
 * io-err: always fails an io, useful for bringing
 * up LV's that have holes in them.
 */
static int io_err_ctr(struct dm_table *t, offset_t b, offset_t l,
		      struct text_region *args, void **result)
{
	/* this takes no arguments */
	*result = 0;
	return 0;
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

