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

static struct target *_targets;
static spinlock_t _lock = SPIN_LOCK_UNLOCKED;

struct target *__get_target(const char *name)
{
	struct target *t;
	for (t = _targets; t && strcmp(t->name, name); t = t->next)
		;
	return t;
}

struct target *dm_get_target(const char *name)
{
	struct target *t;

	spin_lock(&_lock);
	t = __get_target(name);
	spin_unlock(&_lock);

	return t;
}

int register_map_target(const char *name, dm_ctr_fn ctr,
			dm_dtr_fn dtr, dm_map_fn map)
{
	struct target *t = kmalloc(sizeof(*t) + strlen(name) + 1, GFP_KERNEL);

	if (!t)
		return -ENOMEM;

	spin_lock(&_lock);
	if (__get_target(name)) {
		WARN("mapper(%s) already registered\n", name);
		spin_unlock(&_lock);
		return -1;	/* FIXME: what's a good return value ? */
	}

	t->name = (char *) (t + 1);
	strcpy(t->name, name);

	t->ctr = ctr;
	t->dtr = dtr;
	t->map = map;

	t->next = _targets;
	_targets = t;

	spin_unlock(&_lock);
	return 0;
}


/*
 * now for a couple of simple targets:
 *
 * 'io-err' target always fails an io, useful for bringing up LV's
 * that have holes in them.
 *
 * 'linear' target maps a linear range of a device
 */
int io_err_ctr(offset_t b, offset_t e, struct mapped_device *md,
		const char *cb, const char *ce, void **result)
{
	/* this takes no arguments */
	*result = 0;
	return 0;
}

void io_err_dtr(void *c)
{
	/* empty */
}

int io_err_map(struct buffer_head *bh, void *context)
{
	buffer_IO_error(bh);
	return 0;
}


struct linear_c {
	kdev_t dev;
	int offset;		/* FIXME: we need a signed offset type */
};

static int get_number(const char **b, const char *e, unsigned int *n)
{
	char *ptr;
	*b = eat_space(*b, e);
	if (*b >= e)
		return -EINVAL;

	*n = simple_strtoul(*b, &ptr, 10);
	if (ptr == *b)
		return -EINVAL;
	*b = ptr;

	return 0;
}

int linear_ctr(offset_t low, offset_t high, struct mapped_device *md,
	       const char *cb, const char *ce, void **result)
{
	/* context string should be of the form:
	 *  <major> <minor> <offset>
	 */
	struct linear_c *lc;
	unsigned int major, minor, start;
	int r;

	if ((r = get_number(&cb, ce, &major)))
		return r;

	if ((r = get_number(&cb, ce, &minor)))
		return r;

	if ((r = get_number(&cb, ce, &start)))
		return r;

	if (!(lc = kmalloc(sizeof(lc), GFP_KERNEL))) {
		WARN("couldn't allocate memory for linear context\n");
		return -EINVAL;
	}

	lc->dev = MKDEV((int) major, (int) minor);
	lc->offset = (int) start - (int) low;

	if ((r = dm_add_device(md, lc->dev))) {
		kfree(lc);
		return r;
	}

	*result = lc;
	return 0;
}

void linear_dtr(void *c)
{
	kfree(c);
}

int linear_map(struct buffer_head *bh, void *context)
{
	struct linear_c *lc = (struct linear_c *) context;

	bh->b_rdev = lc->dev;
	bh->b_rsector = bh->b_rsector + lc->offset;
	return 1;
}

int dm_std_targets(void)
{
	int ret;

#define xx(n, fn) \
	if ((ret = register_map_target(n, \
             fn ## _ctr, fn ## _dtr, fn ## _map) < 0)) return ret

	xx("io-err", io_err);
	xx("linear", linear);
#undef xx

	return 0;
}
