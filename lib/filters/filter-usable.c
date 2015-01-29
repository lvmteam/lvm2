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
#ifdef UDEV_SYNC_SUPPORT
#include <libudev.h>
#include "dev-ext-udev-constants.h"
#endif

static const char *_too_small_to_hold_pv_msg = "Too small to hold a PV";

static int _native_check_pv_min_size(struct device *dev)
{
	uint64_t size;
	int ret = 0;

	/* Check it's accessible */
	if (!dev_open_readonly_quiet(dev)) {
		log_debug_devs("%s: Skipping: open failed [%s:%p]",
				dev_name(dev), dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	/* Check it's not too small */
	if (!dev_get_size(dev, &size)) {
		log_debug_devs("%s: Skipping: dev_get_size failed [%s:%p]",
				dev_name(dev), dev_ext_name(dev), dev->ext.handle);
		goto out;
	}

	if (size < pv_min_size()) {
		log_debug_devs("%s: Skipping: %s [%s:%p]", dev_name(dev),
				_too_small_to_hold_pv_msg,
				dev_ext_name(dev), dev->ext.handle);
		goto out;
	}

	ret = 1;
out:
	if (!dev_close(dev))
		stack;

	return ret;
}

#ifdef UDEV_SYNC_SUPPORT
static int _udev_check_pv_min_size(struct device *dev)
{
	struct dev_ext *ext;
	const char *size_str;
	char *endp;
	uint64_t size;

	if (!(ext = dev_ext_get(dev)))
		return_0;

	if (!(size_str = udev_device_get_sysattr_value((struct udev_device *)ext->handle, DEV_EXT_UDEV_SYSFS_ATTR_SIZE))) {
		log_debug_devs("%s: Skipping: failed to get size from sysfs [%s:%p]",
				dev_name(dev), dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	errno = 0;
	size = strtoull(size_str, &endp, 10);
	if (errno || !endp || *endp) {
		log_debug_devs("%s: Skipping: failed to parse size from sysfs [%s:%p]",
				dev_name(dev), dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	if (size < pv_min_size()) {
		log_debug_devs("%s: Skipping: %s [%s:%p]", dev_name(dev),
				_too_small_to_hold_pv_msg,
				dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	return 1;
}
#else
static int _udev_check_pv_min_size(struct device *dev)
{
	return 1;
}
#endif

static int _check_pv_min_size(struct device *dev)
{
	if (dev->ext.src == DEV_EXT_NONE)
		return _native_check_pv_min_size(dev);

	if (dev->ext.src == DEV_EXT_UDEV)
		return _udev_check_pv_min_size(dev);

	log_error(INTERNAL_ERROR "Missing hook for PV min size check "
		  "using external device info source %s", dev_ext_name(dev));

	return 0;
}

static int _passes_usable_filter(struct dev_filter *f, struct device *dev)
{
	filter_mode_t mode = *((filter_mode_t *) f->private);
	struct dev_usable_check_params ucp = {0};
	int r;

	/* check if the device is not too small to hold a PV */
	switch (mode) {
		case FILTER_MODE_NO_LVMETAD:
			/* fall through */
		case FILTER_MODE_PRE_LVMETAD:
			if (!_check_pv_min_size(dev))
				return 0;
			break;
		case FILTER_MODE_POST_LVMETAD:
			/* nothing to do here */
			break;
	}

	/* further checks are done on dm devices only */
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
			/*
			 * If we're scanning for lvmetad update,
			 * we don't want to hang on blocked/suspended devices.
			 * When the device is unblocked/resumed, surely,
			 * there's going to be a CHANGE event so the device
			 * gets scanned via udev rule anyway after resume.
			 */
			ucp.check_blocked = 1;
			ucp.check_suspended = 1;
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
		dm_free(f);
		return NULL;
	}
	*((filter_mode_t *) f->private) = mode;

	log_debug_devs("Usable device filter initialised.");

	return f;
}
