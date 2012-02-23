/*
 * Copyright (C) 2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "libdevmapper.h"

#include <stdio.h>
#include <string.h>

#include <CUnit/CUnit.h>

int string_init(void);
int string_fini(void);

static struct dm_pool *mem = NULL;

int string_init(void)
{
	mem = dm_pool_create("string test", 1024);

	return (mem == NULL);
}

int string_fini(void)
{
	dm_pool_destroy(mem);

	return 0;
}

/* TODO: Add more string unit tests here */

static void test_strncpy(void)
{
	const char st[] = "1234567890";
	char buf[sizeof(st)];

	CU_ASSERT_EQUAL(dm_strncpy(buf, st, sizeof(buf)), 1);
	CU_ASSERT_EQUAL(strcmp(buf, st), 0);

	CU_ASSERT_EQUAL(dm_strncpy(buf, st, sizeof(buf) - 1), 0);
	CU_ASSERT_EQUAL(strlen(buf) + 1, sizeof(buf) - 1);
}

static void test_asprint(void)
{
	const char st0[] = "";
	const char st1[] = "12345678901";
	const char st2[] = "1234567890123456789012345678901234567890123456789012345678901234567";
	char *buf;
	int a;

	a = dm_asprintf(&buf, "%s", st0);
	CU_ASSERT_EQUAL(strcmp(buf, st0), 0);
	CU_ASSERT_EQUAL(a, sizeof(st0));
	free(buf);

	a = dm_asprintf(&buf, "%s", st1);
	CU_ASSERT_EQUAL(strcmp(buf, st1), 0);
	CU_ASSERT_EQUAL(a, sizeof(st1));
	free(buf);

	a = dm_asprintf(&buf, "%s", st2);
	CU_ASSERT_EQUAL(a, sizeof(st2));
	CU_ASSERT_EQUAL(strcmp(buf, st2), 0);
	free(buf);
}

CU_TestInfo string_list[] = {
	{ (char*)"asprint", test_asprint },
	{ (char*)"strncpy", test_strncpy },
	CU_TEST_INFO_NULL
};
