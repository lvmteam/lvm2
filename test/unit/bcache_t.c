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

#include "units.h"
#include "bcache.h"

int bcache_init(void)
{
	return 0;
}

int bcache_fini(void)
{
	return 0;
}

static void test_create(void)
{
	struct bcache *cache = bcache_create(8, 16);
	CU_ASSERT_PTR_NOT_NULL(cache);
	bcache_destroy(cache);
}

CU_TestInfo bcache_list[] = {
	{ (char*)"create", test_create },
	CU_TEST_INFO_NULL
};
