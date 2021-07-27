/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/filters/filter.h"

#ifdef __linux__

#include <dirent.h>

static int _ignore_mpath_component(struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name)
{
	dev->filtered_flags &= ~DEV_FILTERED_MPATH_COMPONENT;

	if (dev_is_mpath_component(cmd, dev)) {
		log_debug_devs("%s: Skipping mpath component device", dev_name(dev));
		dev->filtered_flags |= DEV_FILTERED_MPATH_COMPONENT;
		return 0;
	}

	return 1;
}

static void _destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying mpath filter while in use %u times.", f->use_count);
}

struct dev_filter *mpath_filter_create(struct dev_types *dt)
{
	struct dev_filter *f;
	const char *sysfs_dir = dm_sysfs_dir();

	if (!*sysfs_dir) {
		log_verbose("No proc filesystem found: skipping multipath filter");
		return NULL;
	}

	if (!(f = zalloc(sizeof(*f)))) {
		log_error("mpath filter allocation failed");
		return NULL;
	}

	f->passes_filter = _ignore_mpath_component;
	f->destroy = _destroy;
	f->use_count = 0;
	f->name = "mpath";

	log_debug_devs("mpath filter initialised.");

	return f;
}

#else

struct dev_filter *mpath_filter_create(struct dev_types *dt)
{
	return NULL;
}

#endif
