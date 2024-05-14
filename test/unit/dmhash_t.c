/*
 * Copyright (C) 2024 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "units.h"
#include "device_mapper/all.h"

static void test_hash_insert(void *fixture)
{
	static const char _keys[] = { '1', '2', '3', '4', '5' };
	static const long _vals[] = { 'a', 'b', 'c', 'd', 'e' };
	struct dm_hash_node *node;
	unsigned i;
	struct dm_hash_table *hash = dm_hash_create(10);

	T_ASSERT(hash);

	for (i = 0; i < DM_ARRAY_SIZE(_keys); i++)
		T_ASSERT(dm_hash_insert_binary(hash, &_keys[i], sizeof(_keys[0]), (void*)_vals[i]));

	T_ASSERT(dm_hash_get_num_entries(hash) == DM_ARRAY_SIZE(_keys));

	/* list unsorted elements */
	for (node = dm_hash_get_first(hash);  node; node = dm_hash_get_next(hash, node), --i) {
		const char *k = dm_hash_get_key(hash, node);
		char key = k[0];
		void *d = dm_hash_get_data(hash, node);
		//long v = (long) d;
		//printf("key: %c  val: %c\n", key, (char)v);

		T_ASSERT(d == dm_hash_lookup_binary(hash, &key, sizeof(key)));
	}

	T_ASSERT(i == 0);
	dm_hash_destroy(hash);
}

#define T(path, desc, fn) register_test(ts, "/base/data-struct/hash/" path, desc, fn)

void dm_hash_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("insert", "inserting hash elements", test_hash_insert);

	dm_list_add(all_tests, &ts->list);
}
