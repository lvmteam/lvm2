/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
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
#include "lib/commands/toolcontext.h"

static int _passes_deviceid_filter(struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name)
{
	dev->filtered_flags &= ~DEV_FILTERED_DEVICES_FILE;
	dev->filtered_flags &= ~DEV_FILTERED_DEVICES_LIST;

	if (!cmd->enable_devices_file && !cmd->enable_devices_list)
		return 1;

	if (cmd->filter_deviceid_skip)
		return 1;

	if (dev->flags & DEV_MATCHED_USE_ID)
		return 1;

	if (cmd->enable_devices_file)
		dev->filtered_flags |= DEV_FILTERED_DEVICES_FILE;
	else if (cmd->enable_devices_list)
		dev->filtered_flags |= DEV_FILTERED_DEVICES_LIST;

	log_debug_devs("%s: Skipping (deviceid)", dev_name(dev));
	return 0;
}

static void _destroy_deviceid_filter(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying deviceid filter while in use %u times.", f->use_count);

	free(f);
}

struct dev_filter *deviceid_filter_create(struct cmd_context *cmd)
{
	struct dev_filter *f;

	if (!(f = zalloc(sizeof(struct dev_filter)))) {
		log_error("deviceid filter allocation failed");
		return NULL;
	}

	f->passes_filter = _passes_deviceid_filter;
	f->destroy = _destroy_deviceid_filter;
	f->use_count = 0;
	f->name = "deviceid";

	log_debug_devs("deviceid filter initialised.");

	return f;
}
