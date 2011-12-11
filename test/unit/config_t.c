/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
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
#include <CUnit/CUnit.h>

static struct dm_pool *mem;

int config_init() {
	mem = dm_pool_create("config test", 1024);
	return mem == NULL;
}

int config_fini() {
	dm_pool_destroy(mem);
	return 0;
}

static void test_parse()
{
	const char *conf =
		"id = \"yada-yada\"\n"
		"seqno = 15\n"
		"status = [\"READ\", \"WRITE\"]\n"
		"flags = []\n"
		"extent_size = 8192\n"
		"physical_volumes {\n"
		"    pv0 {\n"
		"        id = \"abcd-efgh\"\n"
		"    }\n"
		"    pv1 {\n"
		"        id = \"bbcd-efgh\"\n"
		"    }\n"
		"    pv2 {\n"
		"        id = \"cbcd-efgh\"\n"
		"    }\n"
		"}\n";
	struct dm_config_tree *tree = dm_config_from_string(conf);
	struct dm_config_value *value;

	CU_ASSERT(tree);
	CU_ASSERT(dm_config_has_node(tree->root, "id"));
	CU_ASSERT(dm_config_has_node(tree->root, "physical_volumes"));
	CU_ASSERT(dm_config_has_node(tree->root, "physical_volumes/pv0"));
	CU_ASSERT(dm_config_has_node(tree->root, "physical_volumes/pv0/id"));

	CU_ASSERT(!strcmp(dm_config_find_str(tree->root, "id", "foo"), "yada-yada"));
	CU_ASSERT(!strcmp(dm_config_find_str(tree->root, "idt", "foo"), "foo"));

	CU_ASSERT(!strcmp(dm_config_find_str(tree->root, "physical_volumes/pv0/bb", "foo"), "foo"));
	CU_ASSERT(!strcmp(dm_config_find_str(tree->root, "physical_volumes/pv0/id", "foo"), "abcd-efgh"));

	CU_ASSERT(!dm_config_get_uint32(tree->root, "id", NULL));
	CU_ASSERT(dm_config_get_uint32(tree->root, "extent_size", NULL));

	/* FIXME: Currently everything parses as a list, even if it's not */
	// CU_ASSERT(!dm_config_get_list(tree->root, "id", NULL));
	// CU_ASSERT(!dm_config_get_list(tree->root, "extent_size", NULL));

	CU_ASSERT(dm_config_get_list(tree->root, "flags", &value));
	CU_ASSERT(value->next == NULL);
	CU_ASSERT(dm_config_get_list(tree->root, "status", &value));
	CU_ASSERT(value->next != NULL);

	dm_config_destroy(tree);
}

CU_TestInfo config_list[] = {
	{ (char*)"parse", test_parse },
	CU_TEST_INFO_NULL
};

