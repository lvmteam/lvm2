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

/* similar to ceiling(log_size(n)) */
static uint int_log(ulong n, ulong base)
{
	int result = 0;

	while (n > 1) {
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
	for (; l < t->depth - 1; l++)
		n = get_child(n, CHILDREN_PER_NODE - 1);

	if (n >= t->counts[l])
		return (offset_t) -1;

	return get_node(t, l, n)[KEYS_PER_NODE - 1];
}

/*
 * fills in a level of the btree based on the
 * highs of the level below it.
 */
static int setup_btree_index(int l, struct dm_table *t)
{
	int n, k;
	offset_t *node;

	for (n = 0; n < t->counts[l]; n++) {
		node = get_node(t, l, n);

		for (k = 0; k < KEYS_PER_NODE; k++)
			node[k] = high(t, l + 1, get_child(n, k));
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
	int size = (sizeof(struct target) + sizeof(offset_t)) * num;

	n_highs = vmalloc(size);
	if (n_highs == NULL)
		return -ENOMEM;

	n_targets = (struct target *)(n_highs + num);

	if (n) {
		memcpy(n_highs, t->highs, sizeof(*n_highs) * n);
		memcpy(n_targets, t->targets, sizeof(*n_targets) * n);
	}

	vfree(t->highs);

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

	atomic_set(&t->pending, 0);
	init_waitqueue_head(&t->wait);

	/* allocate a single nodes worth of targets to
	   begin with */
	t->hardsect_size = PAGE_CACHE_SIZE;
	if (alloc_targets(t, KEYS_PER_NODE)) {
		kfree(t);
		t = 0;
	}

	return t;
}

void dm_table_destroy(struct dm_table *t)
{
	int i;

	if (!t)
		return;

	/* free the indexes */
	for (i = 0; i < t->depth - 1; i++) {
		vfree(t->index[i]);
		t->index[i] = 0;
	}
	vfree(t->highs);

	/* free the targets */
	for (i = 0; i < t->num_targets; i++) {
		struct target *tgt = &t->targets[i];
		if (tgt->private)
			tgt->type->dtr(t, tgt->private);
	}
	vfree(t->targets);

	kfree(t);
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
int dm_table_add_target(struct dm_table *t, offset_t high,
			struct target_type *type, void *private)
{
	int r, n;

	if ((r = check_space(t)))
		return r;

	n = t->num_targets++;
	t->highs[n] = high;
	t->targets[n].type = type;
	t->targets[n].private = private;

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
	t->depth = 1 + int_log(leaf_nodes, CHILDREN_PER_NODE);

	/* leaf layer has already been set up */
	t->counts[t->depth - 1] = leaf_nodes;
	t->index[t->depth - 1] = t->highs;

	/* set up internal nodes, bottom-up */
	for (i = t->depth - 2; i >= 0; i--) {
		t->counts[i] = div_up(t->counts[i + 1], CHILDREN_PER_NODE);
		t->index[i] = vmalloc(NODE_SIZE * t->counts[i]);
		if (!t->index[i])
			goto free_indices;
		setup_btree_index(i, t);
	}

	return 0;

free_indices:
	for(++i; i < t->depth - 1; i++) {
		vfree(t->index[i]);
	}
	return -ENOMEM;
}

