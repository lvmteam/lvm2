/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "bitset.h"

/* FIXME: calculate this. */
#define INT_SHIFT 5

bitset_t bitset_create(struct pool *mem, unsigned num_bits)
{
	unsigned n = (num_bits / BITS_PER_INT) + 2;
	size_t size = sizeof(int) * n;
	bitset_t bs;
	
	if (mem)
		bs = pool_zalloc(mem, size);
	else
		bs = dbg_malloc(size);

	if (!bs)
		return NULL;

	*bs = num_bits;

	if (!mem)
		bit_clear_all(bs);

	return bs;
}

void bitset_destroy(bitset_t bs)
{
	dbg_free(bs);
}

void bit_union(bitset_t out, bitset_t in1, bitset_t in2)
{
	int i;
	for (i = (in1[0] / BITS_PER_INT) + 1; i; i--)
		out[i] = in1[i] | in2[i];
}

/*
 * FIXME: slow
 */
static inline int _test_word(uint32_t test, int bit)
{
	while (bit < BITS_PER_INT) {
		if (test & (0x1 << bit))
			return bit;
		bit++;
	}

	return -1;
}

int bit_get_next(bitset_t bs, int last_bit)
{
	int bit, word;
	uint32_t test;

	last_bit++;		/* otherwise we'll return the same bit again */

	while (last_bit < bs[0]) {
		word = last_bit >> INT_SHIFT;
		test = bs[word + 1];
		bit = last_bit & (BITS_PER_INT - 1);

		if ((bit = _test_word(test, bit)) >= 0)
			return (word * BITS_PER_INT) + bit;

		last_bit = last_bit - (last_bit & (BITS_PER_INT - 1)) +
		    BITS_PER_INT;
	}

	return -1;
}

int bit_get_first(bitset_t bs)
{
	return bit_get_next(bs, -1);
}
