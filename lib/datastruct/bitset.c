/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "bitset.h"
#include "dbg_malloc.h"

#include <stdlib.h>

/* FIXME: calculate this. */
#define INT_SHIFT 5

bitset_t bitset_create(struct pool * mem, unsigned num_bits)
{
	int n = (num_bits / BITS_PER_INT) + 2;
	int size = sizeof(int) * n;
	unsigned *bs = pool_zalloc(mem, size);

	if (!bs)
		return NULL;

	*bs = num_bits;
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
