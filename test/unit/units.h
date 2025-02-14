/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
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

#ifndef TEST_UNIT_UNITS_H
#define TEST_UNIT_UNITS_H

#include "framework.h"

//-----------------------------------------------------------------

// Declare the function that adds tests suites here ...
void bcache_tests(struct dm_list *all_tests);
void bcache_utils_tests(struct dm_list *all_tests);
void bitset_tests(struct dm_list *all_tests);
void config_tests(struct dm_list *all_tests);
void dm_list_tests(struct dm_list *all_tests);
void dm_hash_tests(struct dm_list *all_tests);
void dm_status_tests(struct dm_list *all_tests);
void io_engine_tests(struct dm_list *all_tests);
void percent_tests(struct dm_list *all_tests);
void radix_tree_tests(struct dm_list *all_tests);
void regex_tests(struct dm_list *all_tests);
void string_tests(struct dm_list *all_tests);
void vdo_tests(struct dm_list *all_tests);

// ... and call it in here.
static inline void register_all_tests(struct dm_list *all_tests)
{
	bcache_tests(all_tests);
	bcache_utils_tests(all_tests);
	bitset_tests(all_tests);
	config_tests(all_tests);
	dm_list_tests(all_tests);
	dm_hash_tests(all_tests);
	dm_status_tests(all_tests);
	io_engine_tests(all_tests);
	percent_tests(all_tests);
	radix_tree_tests(all_tests);
	regex_tests(all_tests);
	string_tests(all_tests);
	vdo_tests(all_tests);
}

//-----------------------------------------------------------------

#endif
