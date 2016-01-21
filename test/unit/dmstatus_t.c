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

static struct dm_pool *_mem;

int dmstatus_init(void)
{
	_mem = dm_pool_create("dmstatus test", 1024);
	return (_mem == NULL);
}

int dmstatus_fini(void)
{
	dm_pool_destroy(_mem);
	return 0;
}

static void _test_mirror_status(void)
{
	struct dm_status_mirror *s = NULL;

	CU_ASSERT(dm_get_status_mirror(_mem,
				       "2 253:1 253:2 80/81 1 AD 3 disk 253:0 A",
				       &s));
	if (s) {
		CU_ASSERT_EQUAL(s->total_regions, 81);
		CU_ASSERT_EQUAL(s->insync_regions, 80);
		CU_ASSERT_EQUAL(s->dev_count, 2);
		CU_ASSERT_EQUAL(s->devs[0].health, 'A');
		CU_ASSERT_EQUAL(s->devs[0].major, 253);
		CU_ASSERT_EQUAL(s->devs[0].minor, 1);
		CU_ASSERT_EQUAL(s->devs[1].health, 'D');
		CU_ASSERT_EQUAL(s->devs[1].major, 253);
		CU_ASSERT_EQUAL(s->devs[1].minor, 2);
		CU_ASSERT_EQUAL(s->log_count, 1);
		CU_ASSERT_EQUAL(s->logs[0].major, 253);
		CU_ASSERT_EQUAL(s->logs[0].minor, 0);
		CU_ASSERT_EQUAL(s->logs[0].health, 'A');
		CU_ASSERT(!strcmp(s->log_type, "disk"));
	}

	CU_ASSERT(dm_get_status_mirror(_mem,
				       "4 253:1 253:2 253:3 253:4 10/10 1 ADFF 1 core",
				       &s));
	if (s) {
		CU_ASSERT_EQUAL(s->total_regions, 10);
		CU_ASSERT_EQUAL(s->insync_regions, 10);
		CU_ASSERT_EQUAL(s->dev_count, 4);
		CU_ASSERT_EQUAL(s->devs[3].minor, 4);
		CU_ASSERT_EQUAL(s->devs[3].health, 'F');
		CU_ASSERT_EQUAL(s->log_count, 0);
		CU_ASSERT(!strcmp(s->log_type, "core"));
	}
}

CU_TestInfo dmstatus_list[] = {
	{ (char*)"mirror_status", _test_mirror_status },
	CU_TEST_INFO_NULL
};
