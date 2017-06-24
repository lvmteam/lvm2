/*
 * Copyright (C) 2017 Red Hat, Inc. All rights reserved.
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

#include <stdio.h>
#include <string.h>

int percent_init(void)
{
	return 0;
}

int percent_fini(void)
{
	return 0;
}

static void test_percent_100(void)
{
	char buf[32];

        /* Check 100% is shown only for DM_PERCENT_100*/
	dm_percent_t p_100 = dm_make_percent(100, 100);
        dm_percent_t p1_100 = dm_make_percent(100000, 100000);
        dm_percent_t n_100 = dm_make_percent(999999, 1000000);

	CU_ASSERT_EQUAL(p_100, DM_PERCENT_100);
	CU_ASSERT_EQUAL(p1_100, DM_PERCENT_100);
	CU_ASSERT_NOT_EQUAL(n_100, DM_PERCENT_100);

        dm_snprintf(buf, sizeof(buf), "%.2f", dm_percent_to_float(p_100));
	CU_ASSERT_EQUAL(strcmp(buf, "100.00"), 0);

	dm_snprintf(buf, sizeof(buf), "%.2f", dm_percent_to_float(p1_100));
	CU_ASSERT_EQUAL(strcmp(buf, "100.00"), 0);

	dm_snprintf(buf, sizeof(buf), "%.2f", dm_percent_to_float(n_100));
	CU_ASSERT_NOT_EQUAL(strcmp(buf, "99.99"), 0); /* Would like to gett */

	dm_snprintf(buf, sizeof(buf), "%.2f", dm_percent_to_round_float(n_100, 2));
	CU_ASSERT_EQUAL(strcmp(buf, "99.99"), 0);

	dm_snprintf(buf, sizeof(buf), "%.3f", dm_percent_to_round_float(n_100, 3));
	CU_ASSERT_EQUAL(strcmp(buf, "99.999"), 0);

	dm_snprintf(buf, sizeof(buf), "%.4f", dm_percent_to_round_float(n_100, 4));
	CU_ASSERT_EQUAL(strcmp(buf, "99.9999"), 0);

	dm_snprintf(buf, sizeof(buf), "%d", (int)dm_percent_to_round_float(n_100, 0));
	CU_ASSERT_EQUAL(strcmp(buf, "99"), 0);
}

static void test_percent_0(void)
{
	char buf[32];

	/* Check 0% is shown only for DM_PERCENT_0 */
	dm_percent_t p_0 = dm_make_percent(0, 100);
        dm_percent_t p1_0 = dm_make_percent(0, 100000);
        dm_percent_t n_0 = dm_make_percent(1, 1000000);

	CU_ASSERT_EQUAL(p_0, DM_PERCENT_0);
	CU_ASSERT_EQUAL(p1_0, DM_PERCENT_0);
	CU_ASSERT_NOT_EQUAL(n_0, DM_PERCENT_0);

        dm_snprintf(buf, sizeof(buf), "%.2f", dm_percent_to_float(p_0));
	CU_ASSERT_EQUAL(strcmp(buf, "0.00"), 0);

	dm_snprintf(buf, sizeof(buf), "%.2f", dm_percent_to_float(p1_0));
	CU_ASSERT_EQUAL(strcmp(buf, "0.00"), 0);

	dm_snprintf(buf, sizeof(buf), "%.2f", dm_percent_to_float(n_0));
	CU_ASSERT_NOT_EQUAL(strcmp(buf, "0.01"), 0);

	dm_snprintf(buf, sizeof(buf), "%.2f", dm_percent_to_round_float(n_0, 2));
	CU_ASSERT_EQUAL(strcmp(buf, "0.01"), 0);

	dm_snprintf(buf, sizeof(buf), "%.3f", dm_percent_to_round_float(n_0, 3));
	CU_ASSERT_EQUAL(strcmp(buf, "0.001"), 0);

	dm_snprintf(buf, sizeof(buf), "%d", (int)dm_percent_to_round_float(n_0, 0));
	CU_ASSERT_EQUAL(strcmp(buf, "1"), 0);
}

CU_TestInfo percent_list[] = {
	{ (char*)"percent_100", test_percent_100 },
	{ (char*)"percent_0", test_percent_0 },
	CU_TEST_INFO_NULL
};
