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

struct mapped_device *dm_build_btree(struct mapped_device *md)
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
		md->index[i] = __aligned(s, NODE_SIZE);
		memset(md->index[i], -1, s);
	}

	/* bottom layer is easy */
	for (k = md->index[md->depth - 1], i = 0; i < md->num_targets; i++)
		k[i] = t->map[i].high;

	/* fill in higher levels */
	for (i = md->depth - 1; i; i--)
		_setup_btree_index(i - 1, md);

	return 1;
}

void dm_free_btree(struct mapped_device *md)
{
	int i;
	for (i = 0; i < md->depth; i++)
		__free_aligned(md->index[i]);

	__free_aligned(md->targets);
	__free_aligned(md->contexts);
}




