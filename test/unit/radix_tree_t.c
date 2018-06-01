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
#include "base/memory/container_of.h"

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

static void _gen_key(uint8_t *b, uint8_t *e)
{
	for (; b != e; b++)
		*b = rand() % 256;
}

static void test_sparse_keys(void *fixture)
{
	unsigned n;
	struct radix_tree *rt = fixture;
	union radix_value v;
	uint8_t k[32];

	for (n = 0; n < 100000; n++) {
		_gen_key(k, k + sizeof(k));
		v.n = 1234;
		T_ASSERT(radix_tree_insert(rt, k, k + 32, v));
	}
}

static void test_remove_one(void *fixture)
{
	struct radix_tree *rt = fixture;
	uint8_t k[4];
	union radix_value v;

	_gen_key(k, k + sizeof(k));
	v.n = 1234;
	T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));
	T_ASSERT(radix_tree_remove(rt, k, k + sizeof(k)));
	T_ASSERT(!radix_tree_lookup(rt, k, k + sizeof(k), &v));
}

static void test_remove_one_byte_keys(void *fixture)
{
        struct radix_tree *rt = fixture;
        unsigned i, j;
	uint8_t k[1];
	union radix_value v;

	for (i = 0; i < 256; i++) {
        	k[0] = i;
        	v.n = i + 1000;
		T_ASSERT(radix_tree_insert(rt, k, k + 1, v));
	}

	for (i = 0; i < 256; i++) {
        	k[0] = i;
		T_ASSERT(radix_tree_remove(rt, k, k + 1));

		for (j = i + 1; j < 256; j++) {
        		k[0] = j;
			T_ASSERT(radix_tree_lookup(rt, k, k + 1, &v));
			T_ASSERT_EQUAL(v.n, j + 1000);
		}
	}

	for (i = 0; i < 256; i++) {
        	k[0] = i;
		T_ASSERT(!radix_tree_lookup(rt, k, k + 1, &v));
	}
}

static void test_remove_prefix_keys(void *fixture)
{
	struct radix_tree *rt = fixture;
	unsigned i, j;
	uint8_t k[32];
	union radix_value v;

	_gen_key(k, k + sizeof(k));

	for (i = 0; i < 32; i++) {
		v.n = i;
		T_ASSERT(radix_tree_insert(rt, k, k + i, v));
	}

	for (i = 0; i < 32; i++) {
        	T_ASSERT(radix_tree_remove(rt, k, k + i));
        	for (j = i + 1; j < 32; j++) {
                	T_ASSERT(radix_tree_lookup(rt, k, k + j, &v));
                	T_ASSERT_EQUAL(v.n, j);
        	}
	}

        for (i = 0; i < 32; i++)
                T_ASSERT(!radix_tree_lookup(rt, k, k + i, &v));
}

static void test_remove_prefix_keys_reversed(void *fixture)
{
	struct radix_tree *rt = fixture;
	unsigned i, j;
	uint8_t k[32];
	union radix_value v;

	_gen_key(k, k + sizeof(k));

	for (i = 0; i < 32; i++) {
		v.n = i;
		T_ASSERT(radix_tree_insert(rt, k, k + i, v));
	}

	for (i = 0; i < 32; i++) {
        	T_ASSERT(radix_tree_remove(rt, k, k + (31 - i)));
        	for (j = 0; j < 31 - i; j++) {
                	T_ASSERT(radix_tree_lookup(rt, k, k + j, &v));
                	T_ASSERT_EQUAL(v.n, j);
        	}
	}

        for (i = 0; i < 32; i++)
                T_ASSERT(!radix_tree_lookup(rt, k, k + i, &v));
}

static void test_remove_prefix(void *fixture)
{
	struct radix_tree *rt = fixture;
	unsigned i, count = 0;
	uint8_t k[4];
	union radix_value v;

	// populate some random 32bit keys
	for (i = 0; i < 100000; i++) {
        	_gen_key(k, k + sizeof(k));
        	if (k[0] == 21)
                	count++;
		v.n = i;
		T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));
	}

	// remove keys in a sub range 
	k[0] = 21;
	T_ASSERT_EQUAL(radix_tree_remove_prefix(rt, k, k + 1), count);
}

static void test_remove_prefix_single(void *fixture)
{
	struct radix_tree *rt = fixture;
	uint8_t k[4];
	union radix_value v;

	_gen_key(k, k + sizeof(k));
	v.n = 1234;
	T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));
	T_ASSERT_EQUAL(radix_tree_remove_prefix(rt, k, k + 2), 1);
}

static void test_size(void *fixture)
{
	struct radix_tree *rt = fixture;
	unsigned i, dup_count = 0;
	uint8_t k[2];
	union radix_value v;

	// populate some random 16bit keys
	for (i = 0; i < 10000; i++) {
        	_gen_key(k, k + sizeof(k));
        	if (radix_tree_lookup(rt, k, k + sizeof(k), &v))
                	dup_count++;
		v.n = i;
		T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));
	}

	T_ASSERT_EQUAL(radix_tree_size(rt), 10000 - dup_count);
}

struct visitor {
	struct radix_tree_iterator it;
	unsigned count;
};

static bool _visit(struct radix_tree_iterator *it,
                   uint8_t *kb, uint8_t *ke, union radix_value v)
{
	struct visitor *vt = container_of(it, struct visitor, it);
	vt->count++;
	return true;
}

static void test_iterate_all(void *fixture)
{
	struct radix_tree *rt = fixture;
	unsigned i;
	uint8_t k[4];
	union radix_value v;
	struct visitor vt;

	// populate some random 32bit keys
	for (i = 0; i < 100000; i++) {
        	_gen_key(k, k + sizeof(k));
		v.n = i;
		T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));
	}

	vt.count = 0;
	vt.it.visit = _visit;
	radix_tree_iterate(rt, NULL, NULL, &vt.it);
	T_ASSERT_EQUAL(vt.count, radix_tree_size(rt));
}

static void test_iterate_subset(void *fixture)
{
	struct radix_tree *rt = fixture;
	unsigned i, subset_count = 0;
	uint8_t k[3];
	union radix_value v;
	struct visitor vt;

	// populate some random 32bit keys
	for (i = 0; i < 100000; i++) {
        	_gen_key(k, k + sizeof(k));
        	if (k[0] == 21 && k[1] == 12)
                	subset_count++;
		v.n = i;
		T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));
	}

	vt.count = 0;
	vt.it.visit = _visit;
	k[0] = 21;
	k[1] = 12;
	radix_tree_iterate(rt, k, k + 2, &vt.it);
	T_ASSERT_EQUAL(vt.count, subset_count);
}

static void test_iterate_single(void *fixture)
{
	struct radix_tree *rt = fixture;
	uint8_t k[6];
	union radix_value v;
	struct visitor vt;

	_gen_key(k, k + sizeof(k));
	v.n = 1234;
	T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));

	vt.count = 0;
	vt.it.visit = _visit;
	radix_tree_iterate(rt, k, k + 3, &vt.it);
	T_ASSERT_EQUAL(vt.count, 1);
}

static void test_iterate_vary_middle(void *fixture)
{
	struct radix_tree *rt = fixture;
	unsigned i;
	uint8_t k[6];
	union radix_value v;
	struct visitor vt;

	_gen_key(k, k + sizeof(k));
	for (i = 0; i < 16; i++) {
        	k[3] = i;
		v.n = i;
		T_ASSERT(radix_tree_insert(rt, k, k + sizeof(k), v));
	}

	vt.it.visit = _visit;
	for (i = 0; i < 16; i++) {
        	vt.count = 0;
        	k[3] = i;
        	radix_tree_iterate(rt, k, k + 4, &vt.it);
        	T_ASSERT_EQUAL(vt.count, 1);
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
	T("remove-one", "remove one entry", test_remove_one);
	T("remove-one-byte-keys", "remove many one byte keys", test_remove_one_byte_keys);
	T("remove-prefix-keys", "remove a set of keys that have common prefixes", test_remove_prefix_keys);
	T("remove-prefix-keys-reversed", "remove a set of keys that have common prefixes (reversed)", test_remove_prefix_keys_reversed);
	T("remove-prefix", "remove a subrange", test_remove_prefix);
	T("remove-prefix-single", "remove a subrange with a single entry", test_remove_prefix_single);
	T("size-spots-duplicates", "duplicate entries aren't counted twice", test_size);
	T("iterate-all", "iterate all entries in tree", test_iterate_all);
	T("iterate-subset", "iterate a subset of entries in tree", test_iterate_subset);
	T("iterate-single", "iterate a subset that contains a single entry", test_iterate_single);
	T("iterate-vary-middle", "iterate keys that vary in the middle", test_iterate_vary_middle);

	dm_list_add(all_tests, &ts->list);
}
//----------------------------------------------------------------
