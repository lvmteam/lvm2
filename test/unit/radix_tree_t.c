// Copyright (C) 2018 Red Hat, Inc. All rights reserved.
// 
// This file is part of LVM2.
//
// This copyrighted material is made available to anyone wishing to use,
// modify, copy, or redistribute it subject to the terms and conditions
// of the GNU Lesser General Public License v.2.1.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 
#include "base/data-struct/radix-tree.h"

#include "units.h"

#include <stdio.h>
#include <stdlib.h>

//----------------------------------------------------------------

static void *rt_init(void)
{
	struct radix_tree *rt = radix_tree_create(NULL, NULL);
	T_ASSERT(rt);
	return rt;
}

static void rt_exit(void *fixture)
{
	radix_tree_destroy(fixture);
}

static void test_create_destroy(void *fixture)
{
	T_ASSERT(fixture);
}

static void test_insert_one(void *fixture)
{
	struct radix_tree *rt = fixture;
	union radix_value v;
	unsigned char k = 'a';
	v.n = 65;
	T_ASSERT(radix_tree_insert(rt, &k, &k + 1, v));
	v.n = 0;
	T_ASSERT(radix_tree_lookup(rt, &k, &k + 1, &v));
	T_ASSERT_EQUAL(v.n, 65);
}

static void test_single_byte_keys(void *fixture)
{
	unsigned i, count = 256;
	struct radix_tree *rt = fixture;
	union radix_value v;
	uint8_t k;

	for (i = 0; i < count; i++) {
		k = i;
		v.n = 100 + i;
		T_ASSERT(radix_tree_insert(rt, &k, &k + 1, v));
	}

	for (i = 0; i < count; i++) {
		k = i;
		T_ASSERT(radix_tree_lookup(rt, &k, &k + 1, &v));
		T_ASSERT_EQUAL(v.n, 100 + i);
	}
}

static void test_overwrite_single_byte_keys(void *fixture)
{
	unsigned i, count = 256;
	struct radix_tree *rt = fixture;
	union radix_value v;
	uint8_t k;

	for (i = 0; i < count; i++) {
		k = i;
		v.n = 100 + i;
		T_ASSERT(radix_tree_insert(rt, &k, &k + 1, v));
	}

	for (i = 0; i < count; i++) {
		k = i;
		v.n = 1000 + i;
		T_ASSERT(radix_tree_insert(rt, &k, &k + 1, v));
	}

	for (i = 0; i < count; i++) {
		k = i;
		T_ASSERT(radix_tree_lookup(rt, &k, &k + 1, &v));
		T_ASSERT_EQUAL(v.n, 1000 + i);
	}
}

static void test_16_bit_keys(void *fixture)
{
	unsigned i, count = 1 << 16;
	struct radix_tree *rt = fixture;
	union radix_value v;
	uint8_t k[2];

	for (i = 0; i < count; i++) {
		k[0] = i / 256;
		k[1] = i % 256;
		v.n = 100 + i;
		T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));
	}

	for (i = 0; i < count; i++) {
		k[0] = i / 256;
		k[1] = i % 256;
		T_ASSERT(radix_tree_lookup(rt, k, k + sizeof(k), &v));
		T_ASSERT_EQUAL(v.n, 100 + i);
	}
}

static void test_prefix_keys(void *fixture)
{
	struct radix_tree *rt = fixture;
	union radix_value v;
	uint8_t k[2];

	k[0] = 100;
	k[1] = 200;
	v.n = 1024;
	T_ASSERT(radix_tree_insert(rt, k, k + 1, v));
	v.n = 2345;
	T_ASSERT(radix_tree_insert(rt, k, k + 2, v));
	T_ASSERT(radix_tree_lookup(rt, k, k + 1, &v));
	T_ASSERT_EQUAL(v.n, 1024);
	T_ASSERT(radix_tree_lookup(rt, k, k + 2, &v));
	T_ASSERT_EQUAL(v.n, 2345);
}

static void test_prefix_keys_reversed(void *fixture)
{
	struct radix_tree *rt = fixture;
	union radix_value v;
	uint8_t k[2];

	k[0] = 100;
	k[1] = 200;
	v.n = 1024;
	T_ASSERT(radix_tree_insert(rt, k, k + 2, v));
	v.n = 2345;
	T_ASSERT(radix_tree_insert(rt, k, k + 1, v));
	T_ASSERT(radix_tree_lookup(rt, k, k + 2, &v));
	T_ASSERT_EQUAL(v.n, 1024);
	T_ASSERT(radix_tree_lookup(rt, k, k + 1, &v));
	T_ASSERT_EQUAL(v.n, 2345);
}

static void test_sparse_keys(void *fixture)
{
	unsigned i, n;
	struct radix_tree *rt = fixture;
	union radix_value v;
	uint8_t k[32];

	for (n = 0; n < 100000; n++) {
		for (i = 0; i < 32; i++)
			k[i] = rand() % 256;

		v.n = 1234;
		T_ASSERT(radix_tree_insert(rt, k, k + 32, v));
	}
}

//----------------------------------------------------------------

#define T(path, desc, fn) register_test(ts, "/base/data-struct/radix-tree/" path, desc, fn)

void radix_tree_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(rt_init, rt_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("create-destroy", "create and destroy an empty tree", test_create_destroy);
	T("insert-one", "insert one trivial trivial key", test_insert_one);
	T("insert-single-byte-keys", "inserts many single byte keys", test_single_byte_keys);
	T("overwrite-single-byte-keys", "overwrite many single byte keys", test_overwrite_single_byte_keys);
	T("insert-16-bit-keys", "insert many 16bit keys", test_16_bit_keys);
	T("prefix-keys", "prefixes of other keys are valid keys", test_prefix_keys);
	T("prefix-keys-reversed", "prefixes of other keys are valid keys", test_prefix_keys_reversed);
	T("sparse-keys", "see what the memory usage is for sparsely distributed keys", test_sparse_keys);

	dm_list_add(all_tests, &ts->list);
}
//----------------------------------------------------------------
