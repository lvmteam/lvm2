/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "libdevmapper.h"
#include <CUnit/CUnit.h>

int bitset_init(void);
int bitset_fini(void);

enum {
        NR_BITS = 137
};

static struct dm_pool *mem;

int bitset_init(void) {
	mem = dm_pool_create("bitset test", 1024);
	return mem == NULL;
}

int bitset_fini(void) {
	dm_pool_destroy(mem);
	return 0;
}

static void test_get_next(void)
{
        int i, j, last = 0, first;
        dm_bitset_t bs = dm_bitset_create(mem, NR_BITS);

        for (i = 0; i < NR_BITS; i++)
                CU_ASSERT(!dm_bit(bs, i));

        for (i = 0, j = 1; i < NR_BITS; i += j, j++)
                dm_bit_set(bs, i);

        first = 1;
        for (i = 0, j = 1; i < NR_BITS; i += j, j++) {
                if (first) {
                        last = dm_bit_get_first(bs);
                        first = 0;
                } else
                        last = dm_bit_get_next(bs, last);

                CU_ASSERT(last == i);
        }

        CU_ASSERT(dm_bit_get_next(bs, last) == -1);
}

static void bit_flip(dm_bitset_t bs, int bit)
{
        int old = dm_bit(bs, bit);
        if (old)
                dm_bit_clear(bs, bit);
        else
                dm_bit_set(bs, bit);
}

static void test_equal(void)
{
        dm_bitset_t bs1 = dm_bitset_create(mem, NR_BITS);
        dm_bitset_t bs2 = dm_bitset_create(mem, NR_BITS);

        int i, j;
        for (i = 0, j = 1; i < NR_BITS; i += j, j++) {
                dm_bit_set(bs1, i);
                dm_bit_set(bs2, i);
        }

        CU_ASSERT(dm_bitset_equal(bs1, bs2));
        CU_ASSERT(dm_bitset_equal(bs2, bs1));

        for (i = 0; i < NR_BITS; i++) {
                bit_flip(bs1, i);
                CU_ASSERT(!dm_bitset_equal(bs1, bs2));
                CU_ASSERT(!dm_bitset_equal(bs2, bs1));

                CU_ASSERT(dm_bitset_equal(bs1, bs1)); /* comparing with self */
                bit_flip(bs1, i);
        }
}

static void test_and(void)
{
        dm_bitset_t bs1 = dm_bitset_create(mem, NR_BITS);
        dm_bitset_t bs2 = dm_bitset_create(mem, NR_BITS);
        dm_bitset_t bs3 = dm_bitset_create(mem, NR_BITS);

        int i, j;
        for (i = 0, j = 1; i < NR_BITS; i += j, j++) {
                dm_bit_set(bs1, i);
                dm_bit_set(bs2, i);
        }

        dm_bit_and(bs3, bs1, bs2);

        CU_ASSERT(dm_bitset_equal(bs1, bs2));
        CU_ASSERT(dm_bitset_equal(bs1, bs3));
        CU_ASSERT(dm_bitset_equal(bs2, bs3));

        dm_bit_clear_all(bs1);
        dm_bit_clear_all(bs2);

        for (i = 0; i < NR_BITS; i++) {
                if (i % 2)
                        dm_bit_set(bs1, i);
                else
                        dm_bit_set(bs2, i);
        }

        dm_bit_and(bs3, bs1, bs2);
        for (i = 0; i < NR_BITS; i++)
                CU_ASSERT(!dm_bit(bs3, i));
}

CU_TestInfo bitset_list[] = {
	{ (char*)"get_next", test_get_next },
	{ (char*)"equal", test_equal },
	{ (char*)"and", test_and },
	CU_TEST_INFO_NULL
};
