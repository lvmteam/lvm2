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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "filter.h"

static int _passes_lvm_type_device_filter(struct dev_filter *f, struct device *dev)
{
	struct dev_types *dt = (struct dev_types *) f->private;
	const char *name = dev_name(dev);
	int ret = 0;
	uint64_t size;

	/* Is this a recognised device type? */
	if (!dt->dev_type_array[MAJOR(dev->dev)].max_partitions) {
		log_debug_devs("%s: Skipping: Unrecognised LVM device type %"
			       PRIu64, name, (uint64_t) MAJOR(dev->dev));
		return 0;
	}

	/* Check it's accessible */
	if (!dev_open_readonly_quiet(dev)) {
		log_debug_devs("%s: Skipping: open failed", name);
		return 0;
	}

	/* Check it's not too small */
	if (!dev_get_size(dev, &size)) {
		log_debug_devs("%s: Skipping: dev_get_size failed", name);
		goto out;
	}

	if (size < pv_min_size()) {
		log_debug_devs("%s: Skipping: Too small to hold a PV", name);
		goto out;
	}

	if (dev_is_partitioned(dt, dev)) {
		log_debug_devs("%s: Skipping: Partition table signature found",
			       name);
		goto out;
	}

	ret = 1;

      out:
	if (!dev_close(dev))
		stack;

	return ret;
}

static void _lvm_type_filter_destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying lvm_type filter while in use %u times.", f->use_count);

	dm_free(f);
}

struct dev_filter *lvm_type_filter_create(struct dev_types *dt)
{
	struct dev_filter *f;

	if (!(f = dm_zalloc(sizeof(struct dev_filter)))) {
		log_error("LVM type filter allocation failed");
		return NULL;
	}

	f->passes_filter = _passes_lvm_type_device_filter;
	f->destroy = _lvm_type_filter_destroy;
	f->use_count = 0;
	f->private = dt;

	return f;
}
