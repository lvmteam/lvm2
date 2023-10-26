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
#include "device_mapper/all.h"

static void *_mem_init(void)
{
	struct dm_pool *mem = dm_pool_create("dmstatus test", 1024);
	if (!mem) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	return mem;
}

static void _mem_exit(void *mem)
{
	dm_pool_destroy(mem);
}

static void _test_mirror_status(void *fixture)
{
	struct dm_pool *mem = fixture;
	struct dm_status_mirror *s = NULL;

	T_ASSERT(dm_get_status_mirror(mem,
				       "2 253:1 253:2 80/81 1 AD 3 disk 253:0 A",
				       &s));
	if (s) {
		T_ASSERT_EQUAL(s->total_regions, 81);
		T_ASSERT_EQUAL(s->insync_regions, 80);
		T_ASSERT_EQUAL(s->dev_count, 2);
		T_ASSERT_EQUAL(s->devs[0].health, 'A');
		T_ASSERT_EQUAL(s->devs[0].major, 253);
		T_ASSERT_EQUAL(s->devs[0].minor, 1);
		T_ASSERT_EQUAL(s->devs[1].health, 'D');
		T_ASSERT_EQUAL(s->devs[1].major, 253);
		T_ASSERT_EQUAL(s->devs[1].minor, 2);
		T_ASSERT_EQUAL(s->log_count, 1);
		T_ASSERT_EQUAL(s->logs[0].major, 253);
		T_ASSERT_EQUAL(s->logs[0].minor, 0);
		T_ASSERT_EQUAL(s->logs[0].health, 'A');
		T_ASSERT(!strcmp(s->log_type, "disk"));
	}

	T_ASSERT(dm_get_status_mirror(mem,
				       "4 253:1 253:2 253:3 253:4 10/10 1 ADFF 1 core",
				       &s));
	if (s) {
		T_ASSERT_EQUAL(s->total_regions, 10);
		T_ASSERT_EQUAL(s->insync_regions, 10);
		T_ASSERT_EQUAL(s->dev_count, 4);
		T_ASSERT_EQUAL(s->devs[3].minor, 4);
		T_ASSERT_EQUAL(s->devs[3].health, 'F');
		T_ASSERT_EQUAL(s->log_count, 0);
		T_ASSERT(!strcmp(s->log_type, "core"));
	}
}

static void _test_raid_status(void *fixture)
{
	struct dm_pool *mem = fixture;
	struct dm_status_raid *s = NULL;

	T_ASSERT(dm_get_status_raid(mem,
				    "raid6_zr 5 AAAAA 48/68 idle 10 20 -",
				    &s));
	if (s) {
		T_ASSERT_EQUAL(s->total_regions, 68);
		T_ASSERT_EQUAL(s->insync_regions, 48);
		T_ASSERT_EQUAL(s->dev_count, 5);
		T_ASSERT(!strcmp(s->raid_type, "raid6_zr"));
		T_ASSERT(!strcmp(s->dev_health, "AAAAA"));
		T_ASSERT(!strcmp(s->sync_action, "idle"));
		T_ASSERT_EQUAL(s->mismatch_count, 10);
		T_ASSERT_EQUAL(s->data_offset, 20);
	}

	/* remap aa  to Aa */
	T_ASSERT(dm_get_status_raid(mem,
				    "raid1 2 aa 10/20 resync 0 0 -",
				    &s));
	if (s) {
		T_ASSERT_EQUAL(s->total_regions, 20);
		T_ASSERT_EQUAL(s->insync_regions, 10);
		T_ASSERT_EQUAL(s->dev_count, 2);
		T_ASSERT(!strcmp(s->raid_type, "raid1"));
		T_ASSERT(!strcmp(s->dev_health, "Aa"));
		T_ASSERT(!strcmp(s->sync_action, "resync"));
	}

}

void dm_status_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_mem_init, _mem_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	register_test(ts, "/device-mapper/mirror/status", "parsing mirror status", _test_mirror_status);
	register_test(ts, "/device-mapper/raid/status", "parsing raid status", _test_raid_status);
	dm_list_add(all_tests, &ts->list);
}

