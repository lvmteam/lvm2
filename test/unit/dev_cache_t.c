/*
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

#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "units.h"

static void *_fixture_init(void)
{
	struct cmd_context *cmd = calloc(1, sizeof(*cmd));

	if (!cmd)
		goto bad;

	dm_strncpy(cmd->dev_dir, "/dev/", sizeof(cmd->dev_dir));
	cmd->cft = dm_config_from_string("devices { preferred_names = [] }");
	if (!cmd->cft || !dev_cache_init(cmd))
		goto bad;

	return cmd;
bad:
	fprintf(stderr, "could not initialize device cache test\n");
	exit(1);
}

static void _fixture_exit(void *fixture)
{
	struct cmd_context *cmd = fixture;

	dev_cache_exit();
	dm_config_destroy(cmd->cft);
	free(cmd);
}

static void test_devlinks_preserve_preferred_name(void *fixture)
{
	struct device dev = { 0 };
	struct dm_str_list *sl;
	unsigned aliases_found = 0;

	/* Only alias strings are used; no device nodes are opened or scanned. */
	dev_init(&dev);
	T_ASSERT(dev_cache_add_alias(&dev, "/dev/dm-0"));

	/* DEVLINKS may advertise these names before udev creates the symlinks. */
	T_ASSERT(dev_cache_add_alias(&dev, "/dev/mapper/cryptlvm"));
	T_ASSERT(!strcmp(dev_name(&dev), "/dev/dm-0"));
	T_ASSERT(dev_cache_add_alias(&dev, "/dev/disk/by-id/lvm-test"));
	T_ASSERT(!strcmp(dev_name(&dev), "/dev/dm-0"));

	/* Retain pending aliases for regex filters, without adding duplicates. */
	T_ASSERT(dev_cache_add_alias(&dev, "/dev/mapper/cryptlvm"));
	T_ASSERT(dev_cache_add_alias(&dev, "/dev/dm-0"));
	T_ASSERT(!strcmp(dev_name(&dev), "/dev/dm-0"));
	T_ASSERT_EQUAL(dm_list_size(&dev.aliases), 3);
	dm_list_iterate_items(sl, &dev.aliases) {
		if (!strcmp(sl->str, "/dev/mapper/cryptlvm"))
			aliases_found |= 1;
		if (!strcmp(sl->str, "/dev/disk/by-id/lvm-test"))
			aliases_found |= 2;
	}
	T_ASSERT_EQUAL(aliases_found, 3);
}

void dev_cache_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_fixture_init, _fixture_exit);

	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	register_test(ts, "/device/dev-cache/devlinks-preferred-name",
		      "pending DEVLINKS aliases preserve the usable device name",
		      test_devlinks_preserve_preferred_name);

	dm_list_add(all_tests, &ts->list);
}
