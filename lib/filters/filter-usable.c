/*
 * Copyright (C) 2014 Red Hat, Inc. All rights reserved.
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
#include "activate.h" /* device_is_usable */

static int _passes_usable_filter(struct dev_filter *f, struct device *dev)
{
	filter_mode_t mode = *((filter_mode_t *) f->private);
	struct dev_usable_check_params ucp;
	int r;

	/* filter only device-mapper devices */
	if (!dm_is_dm_major(MAJOR(dev->dev)))
		return 1;

	switch (mode) {
		case FILTER_MODE_NO_LVMETAD:
			ucp.check_empty = 1;
			ucp.check_blocked = 1;
			ucp.check_suspended = ignore_suspended_devices();
			ucp.check_error_target = 1;
			ucp.check_reserved = 1;
			break;
		case FILTER_MODE_PRE_LVMETAD:
			ucp.check_empty = 1;
			ucp.check_blocked = 0;
			ucp.check_suspended = ignore_suspended_devices();
			ucp.check_error_target = 1;
			ucp.check_reserved = 1;
			break;
		case FILTER_MODE_POST_LVMETAD:
			ucp.check_empty = 0;
			ucp.check_blocked = 1;
			ucp.check_suspended = ignore_suspended_devices();
			ucp.check_error_target = 0;
			ucp.check_reserved = 0;
			break;
	}

	if (!(r = device_is_usable(dev, ucp)))
		log_debug_devs("%s: Skipping unusable device", dev_name(dev));

	return r;
}

static void _usable_filter_destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying usable device filter while in use %u times.", f->use_count);

	dm_free(f->private);
	dm_free(f);
}

struct dev_filter *usable_filter_create(struct dev_types *dt __attribute__((unused)), filter_mode_t mode)
{
	struct dev_filter *f;

	if (!(f = dm_zalloc(sizeof(struct dev_filter)))) {
		log_error("Usable device filter allocation failed");
		return NULL;
	}

	f->passes_filter = _passes_usable_filter;
	f->destroy = _usable_filter_destroy;
	f->use_count = 0;
	if (!(f->private = dm_zalloc(sizeof(filter_mode_t)))) {
		log_error("Usable device filter mode allocation failed");
		return NULL;
	}
	*((filter_mode_t *) f->private) = mode;

	log_debug_devs("Usable device filter initialised.");

	return f;
}
