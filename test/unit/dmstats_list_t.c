/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
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
#include "libdm/libdevmapper.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Long enough that a short fgets() buffer cannot hold a whole row. */
#define STATS_ROW_BUF_LEN_TEST 4096

/*
 * Shared @stats_list responses.  Row format is
 *   <id>: <start>+<len> <step> <program_id> <aux_data> [args...]
 * and a group tag in aux_data reads DMS_GROUP='<alias>':<members>#<user>.
 */
static const char good_grouped[] =
	"0: 0+512 1 test DMS_GROUP='_g1':0,1#-\n"
	"1: 512+512 1 test -\n"
	"2: 1024+512 1 test -\n";

static const char sparse[] =
	"0: 0+512 1 test DMS_GROUP='_g1':0,5#-\n"
	"5: 2560+512 1 test -\n";

static void *_stats_fixture_init(void)
{
	struct dm_stats *dms;

	dms = dm_stats_create("test");
	if (!dms) {
		fprintf(stderr, "dm_stats_create failed\n");
		exit(1);
	}

	if (!dm_stats_bind_devno(dms, 1, 0)) {
		fprintf(stderr, "dm_stats_bind_devno failed\n");
		exit(1);
	}

	return dms;
}

static void _stats_fixture_exit(void *fixture)
{
	if (fixture)
		dm_stats_destroy(fixture);
}

/*
 * Positive case.  Without it the two rejection tests below pass even if
 * every response is rejected: a parse that reads the rows with the wrong
 * buffer length looks like a rejection, not like a success.
 */
static void test_list_accepts_valid_response(void *fixture)
{
	struct dm_stats *dms = fixture;
	static const char resp[] =
		"0: 0+512 1 test -\n"
		"1: 512+512 1 test -\n"
		"2: 1024+512 1 test -\n";

	T_ASSERT(dm_stats_read_list(dms, resp));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 3);
	T_ASSERT(dm_stats_region_present(dms, 2));
	T_ASSERT(!strcmp(dm_stats_get_region_program_id(dms, 2), "test"));
}

/* Rows are parsed whole: a long row must not be split across reads. */
static void test_list_accepts_long_rows(void *fixture)
{
	struct dm_stats *dms = fixture;
	char resp[STATS_ROW_BUF_LEN_TEST + 128];
	size_t aux_len = 512;
	size_t off = 0;

	/* aux_data longer than any plausible fgets buffer */
	off += (size_t) snprintf(resp + off, sizeof(resp) - off,
				 "0: 0+512 1 test ");
	memset(resp + off, 'x', aux_len);
	off += aux_len;
	off += (size_t) snprintf(resp + off, sizeof(resp) - off, "\n");
	off += (size_t) snprintf(resp + off, sizeof(resp) - off,
				 "1: 512+512 1 test -\n");

	T_ASSERT(dm_stats_read_list(dms, resp));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 2);
	T_ASSERT(strlen(dm_stats_get_region_aux_data(dms, 0)) == aux_len);
}

/* aux_data may contain spaces; the optional args follow it at the end. */
static void test_list_aux_data_with_spaces(void *fixture)
{
	struct dm_stats *dms = fixture;
	static const char resp[] =
		"0: 0+512 1 test my data\n"
		"1: 512+512 1 test my data precise_timestamps\n"
		"2: 1024+512 1 test my data precise_timestamps histogram:0,1\n";

	T_ASSERT(dm_stats_read_list(dms, resp));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 3);
	T_ASSERT(!strcmp(dm_stats_get_region_aux_data(dms, 0), "my data"));
	T_ASSERT(!strcmp(dm_stats_get_region_aux_data(dms, 1), "my data"));
	T_ASSERT(!strcmp(dm_stats_get_region_aux_data(dms, 2), "my data"));
}

/* User aux_data after the group tag may contain spaces. */
static void test_list_group_aux_data_with_spaces(void *fixture)
{
	struct dm_stats *dms = fixture;
	static const char resp[] =
		"0: 0+512 1 test DMS_GROUP='_g1':0,1#user data\n"
		"1: 512+512 1 test -\n";

	T_ASSERT(dm_stats_read_list(dms, resp));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 2);
	T_ASSERT(dm_stats_get_nr_groups(dms) == 1);
	T_ASSERT(!strcmp(dm_stats_get_region_aux_data(dms, 0), "user data"));
}

/* A quoted group alias may contain spaces. */
static void test_list_group_alias_with_spaces(void *fixture)
{
	struct dm_stats *dms = fixture;
	static const char resp[] =
		"0: 0+512 1 test DMS_GROUP='a b':0,1#-\n"
		"1: 512+512 1 test -\n";

	T_ASSERT(dm_stats_read_list(dms, resp));
	T_ASSERT(dm_stats_get_nr_groups(dms) == 1);
	T_ASSERT(!strcmp(dm_stats_get_alias(dms, 0), "a b"));
	T_ASSERT(!strcmp(dm_stats_get_region_aux_data(dms, 0), ""));
}

/* A row must be rejected whole, not partially applied. */
static void test_list_leaves_handle_usable_after_error(void *fixture)
{
	struct dm_stats *dms = fixture;
	static const char good[] =
		"0: 0+512 1 test -\n"
		"1: 512+512 1 test -\n";
	static const char bad[] =
		"2: 0+512 1 test -\n"
		"2: 512+512 1 test -\n";

	T_ASSERT(dm_stats_read_list(dms, good));
	T_ASSERT(!dm_stats_read_list(dms, bad));
	T_ASSERT(!dm_stats_get_nr_regions(dms));

	/* the handle must still accept a valid response */
	T_ASSERT(dm_stats_read_list(dms, good));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 2);
}

/* An empty response leaves the handle listed but empty. */
static void test_list_accepts_empty_response(void *fixture)
{
	struct dm_stats *dms = fixture;

	T_ASSERT(dm_stats_read_list(dms, good_grouped));
	T_ASSERT(dm_stats_read_list(dms, ""));
	T_ASSERT(!dm_stats_get_nr_regions(dms));
	T_ASSERT(!dm_stats_get_nr_groups(dms));
	T_ASSERT(dm_stats_read_list(dms, good_grouped));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 3);
}

/* Re-listing must fully reset region and group state. */
static void test_relist_resets_state(void *fixture)
{
	struct dm_stats *dms = fixture;
	static const char ungrouped[] =
		"0: 0+512 1 test -\n"
		"1: 512+512 1 test -\n";

	T_ASSERT(dm_stats_read_list(dms, good_grouped));
	T_ASSERT(dm_stats_get_nr_groups(dms) == 1);
	T_ASSERT(dm_stats_get_nr_regions(dms) == 3);

	T_ASSERT(dm_stats_read_list(dms, ungrouped));
	T_ASSERT(!dm_stats_get_nr_groups(dms));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 2);
}

/* A group tag whose leader is not the lowest member must be refused. */
static void test_group_descriptor_wrong_leader(void *fixture)
{
	struct dm_stats *dms = fixture;
	static const char resp[] =
		"0: 0+512 1 test -\n"
		"1: 512+512 1 test DMS_GROUP='_g1':0,1#-\n";

	T_ASSERT(dm_stats_read_list(dms, resp));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 2);
	T_ASSERT(!dm_stats_get_nr_groups(dms));
}

/* Region ids with holes must not corrupt the group walk. */
static void test_sparse_region_ids(void *fixture)
{
	struct dm_stats *dms = fixture;
	uint64_t len = 0;

	T_ASSERT(dm_stats_read_list(dms, sparse));
	T_ASSERT(dm_stats_get_nr_regions(dms) == 2);
	T_ASSERT(dm_stats_get_nr_groups(dms) == 1);
	T_ASSERT(dm_stats_get_region_len(dms, &len, DM_STATS_WALK_GROUP));
	T_ASSERT(len == 512 + 512);
}

static void test_list_rejects_out_of_order_ids(void *fixture)
{
	struct dm_stats *dms = fixture;
	static const char resp[] =
		"0: 0+512 1 - -\n"
		"0: 512+512 1 - -\n";

	T_ASSERT(!dm_stats_read_list(dms, resp));
	T_ASSERT(!dm_stats_get_nr_regions(dms));
}

static void test_list_rejects_oversized_region_id(void *fixture)
{
	struct dm_stats *dms = fixture;
	char resp[80];

	snprintf(resp, sizeof(resp), "%llu: 0+512 1 - -\n",
		 (unsigned long long) INT_MAX + 1ULL);

	T_ASSERT(!dm_stats_read_list(dms, resp));
	T_ASSERT(!dm_stats_get_nr_regions(dms));
}

/* Walking an unlisted handle must report the end, not walk off the table. */
static void test_walk_unlisted_handle(void *fixture)
{
	struct dm_stats *dms = fixture;

	dm_stats_walk_init(dms, DM_STATS_WALK_ALL);
	dm_stats_walk_start(dms);
	T_ASSERT(dm_stats_walk_end(dms));
}

/*
 * walk_end() must report the end on an unlisted handle even without a
 * preceding walk_start(), and repeated calls must stay on the empty
 * walk path instead of walking off the table.
 */
static void test_walk_end_unlisted_handle(void *fixture)
{
	struct dm_stats *dms = fixture;

	dm_stats_walk_init(dms, DM_STATS_WALK_ALL);
	T_ASSERT(dm_stats_walk_end(dms));
	T_ASSERT(dm_stats_walk_end(dms));

	dm_stats_walk_start(dms);
	T_ASSERT(dm_stats_walk_end(dms));
	T_ASSERT(dm_stats_walk_end(dms));
}

static void test_get_group_id_unlisted_handle(void *fixture)
{
	struct dm_stats *dms = fixture;

	T_ASSERT(dm_stats_get_group_id(dms, 0) == DM_STATS_GROUP_NONE);
	T_ASSERT(dm_stats_get_group_id(dms, DM_STATS_REGION_CURRENT)
		 == DM_STATS_GROUP_NONE);
}

/* A region id that is absent from the table does not belong to a group. */
static void test_get_group_id_invalid_region(void *fixture)
{
	struct dm_stats *dms = fixture;

	T_ASSERT(dm_stats_read_list(dms, sparse));

	/* region 3 is the hole between the group members */
	T_ASSERT(dm_stats_get_group_id(dms, 0) == 0);
	T_ASSERT(dm_stats_get_group_id(dms, 5) == 0);
	T_ASSERT(dm_stats_get_group_id(dms, 3) == DM_STATS_GROUP_NONE);
	T_ASSERT(dm_stats_get_group_id(dms, 6) == DM_STATS_GROUP_NONE);
}

#define T(path, desc, fn) register_test(ts, "/base/libdm/dmstats-list/" path, desc, fn)

void dmstats_list_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_stats_fixture_init,
						  _stats_fixture_exit);

	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("valid", "accept a well formed @stats_list response",
	  test_list_accepts_valid_response);
	T("long-rows", "parse rows longer than the read buffer",
	  test_list_accepts_long_rows);
	T("aux-data-spaces", "parse aux_data containing spaces",
	  test_list_aux_data_with_spaces);
	T("group-aux-data-spaces", "parse group user aux_data with spaces",
	  test_list_group_aux_data_with_spaces);
	T("group-alias-spaces", "parse a quoted group alias with spaces",
	  test_list_group_alias_with_spaces);
	T("usable-after-error", "handle still works after a rejected response",
	  test_list_leaves_handle_usable_after_error);
	T("empty", "accept an empty @stats_list response",
	  test_list_accepts_empty_response);
	T("out-of-order", "reject duplicate region_id values",
	  test_list_rejects_out_of_order_ids);
	T("oversized-id", "reject region_id above INT_MAX",
	  test_list_rejects_oversized_region_id);
	T("relist", "re-listing resets region and group state",
	  test_relist_resets_state);
	T("group-wrong-leader", "reject a group tag on a non-leader region",
	  test_group_descriptor_wrong_leader);
	T("sparse-ids", "group over region ids with a hole", test_sparse_region_ids);
	T("walk-unlisted", "walk over an unlisted handle reports the end",
	  test_walk_unlisted_handle);
	T("walk-end-unlisted", "walk_end without walk_start reports the end",
	  test_walk_end_unlisted_handle);
	T("group-id-unlisted", "group id lookup on an unlisted handle is NONE",
	  test_get_group_id_unlisted_handle);
	T("group-id-invalid", "group id lookup rejects absent region ids",
	  test_get_group_id_invalid_region);

	dm_list_add(all_tests, &ts->list);
}
