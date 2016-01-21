/*
 * Copyright (C) 2015 Red Hat, Inc. All rights reserved.
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

int dmlist_init(void)
{
	return 0;
}

int dmlist_fini(void)
{
	return 0;
}

static void test_dmlist_splice(void)
{
	struct dm_list a[10];
	struct dm_list list1;
	struct dm_list list2;
	unsigned i;

	dm_list_init(&list1);
	dm_list_init(&list2);

	for (i = 0; i < DM_ARRAY_SIZE(a); i++)
		dm_list_add(&list1, &a[i]);

	dm_list_splice(&list2, &list1);
	CU_ASSERT_EQUAL(dm_list_size(&list1), 0);
	CU_ASSERT_EQUAL(dm_list_size(&list2), 10);
}

CU_TestInfo dmlist_list[] = {
	{ (char*)"dmlist_splice", test_dmlist_splice },
	//{ (char*)"dmlist", test_strncpy },
	CU_TEST_INFO_NULL
};
