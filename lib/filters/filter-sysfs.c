/*
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/filters/filter.h"

static int _sys_dev_block_found;

#ifdef __linux__

static int _accept_p(struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name)
{
	char path[PATH_MAX];
	const char *sysfs_dir;
	struct stat info;

	if (!_sys_dev_block_found)
		return 1;

	dev->filtered_flags &= ~DEV_FILTERED_SYSFS;

	/*
	 * Any kind of device id other than devname has been set
	 * using sysfs so we know that sysfs info exists for dev.
	 */
	if (dev->id && dev->id->idtype && (dev->id->idtype != DEV_ID_TYPE_DEVNAME))
		return 1;

	sysfs_dir = dm_sysfs_dir();
	if (sysfs_dir && *sysfs_dir) {
		if (dm_snprintf(path, sizeof(path), "%sdev/block/%d:%d",
				sysfs_dir, (int)MAJOR(dev->dev), (int)MINOR(dev->dev)) < 0) {
			log_debug("failed to create sysfs path");
			return 1;
		}

		if (lstat(path, &info)) {
			log_debug_devs("%s: Skipping (sysfs)", dev_name(dev));
			dev->filtered_flags |= DEV_FILTERED_SYSFS;
			return 0;
		}
	}

	return 1;
}

static void _destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying sysfs filter while in use %u times.", f->use_count);
	free(f);
}

static void _check_sys_dev_block(void)
{
	char path[PATH_MAX];
	const char *sysfs_dir;
	struct stat info;

	sysfs_dir = dm_sysfs_dir();
	if (sysfs_dir && *sysfs_dir) {
		if (dm_snprintf(path, sizeof(path), "%sdev/block", sysfs_dir) < 0)
			return;

		if (lstat(path, &info)) {
			log_debug("filter-sysfs disabled: /sys/dev/block not found");
			_sys_dev_block_found = 0;
		} else {
			_sys_dev_block_found = 1;
		}
	}
}

struct dev_filter *sysfs_filter_create(void)
{
	const char *sysfs_dir = dm_sysfs_dir();
	struct dev_filter *f;

	if (!*sysfs_dir) {
		log_verbose("No proc filesystem found: skipping sysfs filter");
		return NULL;
	}

	/* support old kernels that don't have this */
	_check_sys_dev_block();

	if (!(f = zalloc(sizeof(*f))))
		goto_bad;

	f->passes_filter = _accept_p;
	f->destroy = _destroy;
	f->use_count = 0;
	f->name = "sysfs";

	log_debug_devs("Sysfs filter initialised.");

	return f;

 bad:
	return NULL;
}

#else

struct dev_filter *sysfs_filter_create(const char *sysfs_dir __attribute__((unused)))
{
	return NULL;
}

#endif
