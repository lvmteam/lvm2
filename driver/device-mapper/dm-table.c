/*
 * dm-table.c
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
 * Changelog
 *
 *     16/08/2001 - First version [Joe Thornber]
 */

#include "dm.h"

/* ceiling(n / size) * size */
static inline ulong round_up(ulong n, ulong size)
{
	ulong r = n % size;
	return n + (r ? (size - r) : 0);
}

/* ceiling(n / size) */
static inline ulong div_up(ulong n, ulong size)
{
	return round_up(n, size) / size;
}

/* ceiling(log_size(n)) */
static uint int_log(ulong n, ulong base)
{
	int result = 0;

	while (n != 1) {
		n = div_up(n, base);
		result++;
	}

	return result;
}

/*
 * return the highest key that you could lookup
 * from the n'th node on level l of the btree.
 */
static offset_t high(struct dm_table *t, int l, int n)
{
	while (1) {
		if (n >= t->counts[l])
			return (offset_t) -1;

		if (l == t->depth - 1)
			return t->index[l][((n + 1) * KEYS_PER_NODE) - 1];

		l++;
		n = (n + 1) * (KEYS_PER_NODE + 1) - 1;
	}

	return -1;
}

/*
 * fills in a level of the btree based on the
 * highs of the level below it.
 */
static int setup_btree_index(int l, struct dm_table *t)
{
	int n, c, cn;

	for (n = 0, cn = 0; n < t->counts[l]; n++) {
		offset_t *k = t->index[l] + (n * KEYS_PER_NODE);

		for (c = 0; c < KEYS_PER_NODE; c++)
			k[c] = high(t, l + 1, cn++);
		cn++;		/* one extra for the child that's
                                   greater than all keys */
	}

	return 0;
}

/*
 * highs, and targets are managed as dynamic
 * arrays during a table load.
 */
static int alloc_targets(struct dm_table *t, int num)
{
	offset_t *n_highs;
	struct target *n_targets;
	int n = t->num_targets;

	if (!(n_highs = vmalloc(sizeof(*n_highs) * num)))
		return -ENOMEM;

	if (!(n_targets = vmalloc(sizeof(*n_targets) * num))) {
		vfree(n_highs);
		return -ENOMEM;
	}

	if (n) {
		memcpy(n_highs, t->highs, sizeof(*n_highs) * n);
		memcpy(n_targets, t->targets, sizeof(*n_targets) * n);
	}

	vfree(t->highs);
	vfree(t->targets);

	t->num_allocated = num;
	t->highs = n_highs;
	t->targets = n_targets;

	return 0;
}

struct dm_table *dm_table_create(void)
{
	struct dm_table *t = kmalloc(sizeof(struct dm_table), GFP_NOIO);

	if (!t)
		return 0;

	memset(t, 0, sizeof(*t));

	/* allocate a single nodes worth of targets to
	   begin with */
	if (t && alloc_targets(t, KEYS_PER_NODE)) {
		kfree(t);
		t = 0;
	}

	return t;
}

void dm_table_destroy(struct dm_table *t)
{
	struct dev_list *d, *n;
	int i;

	if (!t)
		return;

	/* free the indexes */
	for (i = 0; i < t->depth; i++) {
		vfree(t->index[i]);
		t->index[i] = 0;
	}

	/* t->highs was already freed as t->index[t->depth - 1] */
	vfree(t->targets);
	kfree(t);

	/* free the device list */
	for (d = t->devices; d; d = n) {
		n = d->next;
		kfree(d);
	}
}

/*
 * checks to see if we need to extend highs or targets
 */
static inline int check_space(struct dm_table *t)
{
	if (t->num_targets >= t->num_allocated)
		return alloc_targets(t, t->num_allocated * 2);

	return 0;
}

/*
 * adds a target to the map
 */
int dm_table_add_entry(struct dm_table *t, offset_t high,
		       dm_map_fn target, void *private)
{
	int r, n;

	if ((r = check_space(t)))
		return r;

	n = t->num_targets++;
	t->highs[n] = high;
	t->targets[n].map = target;
	t->targets[n].private = private;

	return 0;
}

int dm_table_add_device(struct dm_table *t, kdev_t dev)
{
	struct dev_list *d = kmalloc(sizeof(*d), GFP_KERNEL);

	if (!d)
		return -ENOMEM;

	d->dev = dev;
	d->next = t->devices;
	t->devices = d;

	return 0;
}

/*
 * builds the btree to index the map
 */
int dm_table_complete(struct dm_table *t)
{
	int i, leaf_nodes;

	/* how many indexes will the btree have ? */
	leaf_nodes = div_up(t->num_targets, KEYS_PER_NODE);
	i = 1 + int_log(leaf_nodes, KEYS_PER_NODE + 1);

	/* work out how many nodes are in each layer */
	t->depth = i;
	t->counts[t->depth - 1] = div_up(t->num_targets, KEYS_PER_NODE);

	while (--i)
		t->counts[i - 1] = div_up(t->counts[i], KEYS_PER_NODE + 1);

	/* allocate memory for the internal nodes */
	for (i = 0; i < (t->depth - 1); i++) {
		size_t s = NODE_SIZE * t->counts[i];
		t->index[i] = vmalloc(s);
		memset(t->index[i], -1, s);
	}

	/* leaf layer has already been set up */
	t->index[t->depth - 1] = t->highs;

	/* fill in higher levels */
	for (i = t->depth - 1; i; i--)
		setup_btree_index(i - 1, t);

	return 0;
}


EXPORT_SYMBOL(dm_table_add_device);
