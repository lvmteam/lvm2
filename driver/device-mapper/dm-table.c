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

static inline ulong _round_up(ulong n, ulong size)
{
	ulong r = n % size;
	return n + (r ? (size - r) : 0);
}

static inline ulong _div_up(ulong n, ulong size)
{
	return _round_up(n, size) / size;
}

static offset_t _high(struct mapped_device *md, int l, int n)
{
	while (1) {
		if (n >= md->counts[l])
			return (offset_t) -1;

		if (l == md->depth - 1)
			return md->index[l][((n + 1) * KEYS_PER_NODE) - 1];

		l++;
		n = (n + 1) * (KEYS_PER_NODE + 1) - 1;
	}
}

static int _setup_btree_index(int l, struct mapped_device *md)
{
	int n, c, cn;

	for (n = 0, cn = 0; n < md->counts[l]; n++) {
		offset_t *k = md->index[l] + (n * KEYS_PER_NODE);

		for (c = 0; c < KEYS_PER_NODE; c++)
			k[c] = _high(md, l + 1, cn++);
		cn++;
	}

	return 1;
}

void dm_free_btree(struct mapped_device *md)
{
	int i;
	for (i = 0; i < md->depth; i++)
		__free_aligned(md->index[i]);

	__free_aligned(md->targets);
	__free_aligned(md->contexts);

	md->num_targets = 0;
	md->num_allocated = 0;
}

static int _setup_targets(struct mapped_device *md, struct device_table *t)
{
	int i;
	offset_t low = 0;

	md->num_targets = t->count;
	md->targets = vmalloc(sizeof(*md->targets) * md->num_targets,
			      NODE_SIZE);

	for (i = 0; i < md->num_targets; i++) {
		struct mapper *m = _find_mapper(t->map[i].type);
		if (!m)
			return 0;

		if (!m->ctr(low, t->map[i].high + 1,
			    t->map[i].context, md->contexts + i)) {
			WARN("contructor for '%s' failed", m->name);
			return 0;
		}

		md->targets[i] = m->map;
	}

	return 1;
}

int dm_start_table(struct mapped_device *md)
{
	bit_set(md->state, DM_LOADING);

	dm_free_btree(md);
	if (!_alloc_targets(2))	/* FIXME: increase once debugged 256 ? */
		return 0;
}

int dm_add_entry(struct mapped_device *md, offset_t high,
		 dm_map_fn target, void *context)
{
	if (md->num_targets >= md->num_entries &&
	    !_alloc_targets(md->num_allocated * 2))
		retun -ENOMEM;

	md->highs[md->num_targets] = high;
	md->targets[md->num_targets] = target;
	md->contexts[md->num_targets] = context;

	md->num_targets++;
}

int dm_complete_table(struct mapped_device *md)
{
	int n, i;
	offset_t *k;

	/* how many indexes will the btree have ? */
	for (n = _div_up(md->num_targets, KEYS_PER_NODE), i = 1; n != 1; i++)
		n = _div_up(n, KEYS_PER_NODE + 1);

	md->depth = i;
	md->counts[md->depth - 1] = _div_up(md->num_targets, KEYS_PER_NODE);

	while (--i)
		md->counts[i - 1] = _div_up(md->counts[i], KEYS_PER_NODE + 1);

	for (i = 0; i < md->depth; i++) {
		size_t s = NODE_SIZE * md->counts[i];
		md->index[i] = vmalloc(s);
		memset(md->index[i], -1, s);
	}

	/* bottom layer is easy */
	md->index[md->depth - 1] = md->highs;

	/* fill in higher levels */
	for (i = md->depth - 1; i; i--)
		_setup_btree_index(i - 1, md);

	return 1;
}

static int _alloc_targets(int num)
{
	offset_t *n_highs;
	dm_map_fn *n_targets;
	void **n_contexts;

	if (!(n_highs = vmalloc(sizeof(*n_highs) * num)))
		return 0;

	if (!(n_targets = vmalloc(sizeof(*n_targets) * num))) {
		vfree(n_highs);
		return 0;
	}

	if (!(n_contexts = vmalloc(sizeof(*n_contexts) * num))) {
		vfree(n_highs);
		vfree(n_targets);
		return 0;
	}

	memcpy(n_highs, md->highs, sizeof(*n_highs) * md->num_targets);
	memcpy(n_targets, md->targets, sizeof(*n_targets) * md->num_targets);
	memcpy(n_contexts, md->contexts,
	       sizeof(*n_contexts) * md->num_targets);

	vfree(md->highs);
	vfree(md->targets);
	vfree(md->contexts);

	md->num_allocated = num;
	md->highs = n_highs;
	md->targets = n_targets;
	md->contexts = n_contexts;

	return 1;
}
