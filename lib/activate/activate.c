/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"
#include "ll-activate.h"
#include "display.h"
#include "log.h"
#include "fs.h"
#include "lvm-string.h"
#include "names.h"

#include <limits.h>
#include <linux/kdev_t.h>
#include <fcntl.h>

#define _skip(fmt, args...) log_very_verbose("Skipping: " fmt , ## args)

int library_version(char *version, size_t size)
{
	if (!dm_get_library_version(version, size))
		return 0;
	return 1;
}

int driver_version(char *version, size_t size)
{
	int r = 0;
	struct dm_task *dmt;

	log_very_verbose("Getting driver version");
	if (!(dmt = dm_task_create(DM_DEVICE_VERSION))) {
		stack;
		return 0;
	}

	if (!dm_task_run(dmt))
		log_error("Failed to get driver version");

	if (!dm_task_get_driver_version(dmt, version, size))
		goto out;

	r = 1;

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _query(struct logical_volume *lv, int (*fn)(const char *))
{
	char buffer[128];

	if (!build_dm_name(buffer, sizeof(buffer), "",
			   lv->vg->name, lv->name)) {
		stack;
		return -1;
	}

	return fn(buffer);
}


/*
 * These three functions return the number of LVs in the state,
 * or -1 on error.
 */
int lv_active(struct logical_volume *lv)
{
	return _query(lv, device_active);
}

int lv_suspended(struct logical_volume *lv)
{
	return _query(lv, device_suspended);
}

int lv_open_count(struct logical_volume *lv)
{
	return _query(lv, device_open_count);
}


/*
 * Returns 1 if info structure populated, else 0 on failure.
 */
int lv_info(struct logical_volume *lv, struct dm_info *info)
{
	char buffer[128];

	if (!build_dm_name(buffer, sizeof(buffer), "",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	return device_info(buffer, info);
}

static inline int _read_only_lv(struct logical_volume *lv)
{
	return (lv->status & LVM_WRITE) && (lv->vg->status & LVM_WRITE);
}

int _lv_activate_named(struct logical_volume *lv, const char *name)
{
	int r = 0;
	struct dm_task *dmt;

	if (test_mode()) {
		_skip("Activation of '%s'.", lv->name);
		return 0;
	}

	/*
	 * Create a task.
	 */
	if (!(dmt = setup_dm_task(name, DM_DEVICE_CREATE))) {
		stack;
		return 0;
	}

	/*
	 * Populate it.
	 */
	if (!device_populate_lv(dmt, lv)) {
		stack;
		return 0;
	}

	/*
	 * Do we want a specific minor number ?
	 */
	if (lv->minor >= 0) {
		if (!dm_task_set_minor(dmt, MINOR(lv->minor))) {
			log_error("Failed to set minor number for %s to %d "
				  "during activation.", lv->name, lv->minor);
			goto out;
		} else
			log_very_verbose("Set minor number for %s to %d.",
					 lv->name, lv->minor);
	}

	/*
	 * Read only ?
	 */
	if (!_read_only_lv(lv)) {
	    	if (!dm_task_set_ro(dmt)) {
			log_error("Failed to set %s read-only during "
				  "activation.", lv->name);
			goto out;
		} else
			log_very_verbose("Activating %s read-only", lv->name);
	}

	/*
	 * Load this into the kernel.
	 */
	if (!(r = dm_task_run(dmt))) {
		log_err("Activation failed.");
		goto out;
	}

 out:
	dm_task_destroy(dmt);
	log_verbose("Logical volume %s%s activated", lv->name,
		    r == 1 ? "" : " not");
	return r;
}

int lv_activate(struct logical_volume *lv)
{
	char buffer[128];

	/*
	 * Decide what we're going to call this device.
	 */
	if (!build_dm_name(buffer, sizeof(buffer), "",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	if (!_lv_activate_named(lv, buffer) ||
	    !fs_add_lv(lv, lv->minor)) {
		stack;
		return 0;
	}

	return 1;
}

static int _reload(const char *name, struct logical_volume *lv)
{
	int r = 0;
	struct dm_task *dmt;

	/*
	 * Create a task.
	 */
	if (!(dmt = setup_dm_task(name, DM_DEVICE_RELOAD))) {
		stack;
		return 0;
	}

	/*
	 * Populate it.
	 */
	if (!device_populate_lv(dmt, lv)) {
		stack;
		return 0;
	}

	/*
	 * Load this into the kernel.
	 */
	if (!(r = dm_task_run(dmt))) {
		log_err("Activation failed.");
		goto out;
	}

	/*
	 * Create device nodes and symbolic links.
	 */
	if (!fs_add_lv(lv, lv->minor))
		stack;

 out:
	dm_task_destroy(dmt);
	log_verbose("Logical volume %s%s re-activated", lv->name,
		    r == 1 ? "" : " not");
	return r;
}

int lv_reactivate(struct logical_volume *lv)
{
	int r;
	char buffer[128];

	if (test_mode()) {
		_skip("Reactivation of '%s'.", lv->name);
		return 0;
	}

	/*
	 * Decide what we're going to call this device.
	 */
	if (!build_dm_name(buffer, sizeof(buffer), "",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	/*
	 * Suspend the device if it isn't already.
	 */
	if (!device_suspended(buffer) && !device_suspend(buffer)) {
		stack;
		return 0;
	}

	r = _reload(buffer, lv);

	if (!device_resume(buffer)) {
		stack;
		return 0;
	}

	return r;
}

int lv_deactivate(struct logical_volume *lv)
{
	char buffer[128];

	log_very_verbose("Deactivating %s", lv->name);
	if (test_mode()) {
		_skip("Deactivating '%s'.", lv->name);
		return 0;
	}

	if (!build_dm_name(buffer, sizeof(buffer), "",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	if (!device_deactivate(buffer)) {
		stack;
		return 0;
	}

	fs_del_lv(lv);

	return 1;
}

int lv_suspend(struct logical_volume *lv)
{
	char buffer[128];

	log_very_verbose("Suspending %s", lv->name);
	if (test_mode()) {
		_skip("Suspending '%s'.", lv->name);
		return 0;
	}

	if (!build_dm_name(buffer, sizeof(buffer), "",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	if (!device_suspend(buffer)) {
		stack;
		return 0;
	}

	fs_del_lv(lv);

	return 1;
}

int lv_rename(const char *old_name, struct logical_volume *lv)
{
	int r = 0;
	char new_name[PATH_MAX];
	struct dm_task *dmt;

	if (test_mode()) {
		_skip("Rename '%s' to '%s'.", old_name, lv->name);
		return 0;
	}

	if (!(dmt = setup_dm_task(old_name, DM_DEVICE_RENAME))) {
		stack;
		return 0;
	}

	if (!build_dm_name(new_name, sizeof(new_name), "",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	if (!dm_task_set_newname(dmt, new_name)) {
		stack;
		r = 0;
		goto end;
	}

	if (!dm_task_run(dmt)) {
		stack;
		r = 0;
		goto end;
	}

	fs_rename_lv(old_name, lv);

 end:
	dm_task_destroy(dmt);
	return r;
}

/*
 * Zero the start of a cow store so the driver spots that it is a
 * new store.
 */
int lv_setup_cow_store(struct logical_volume *lv)
{
	char buffer[128];
	char path[PATH_MAX];
	struct device *dev;

	/*
	 * Decide what we're going to call this device.
	 */
	if (!build_dm_name(buffer, sizeof(buffer), "cow_init",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	if (!_lv_activate_named(lv, buffer)) {
		log_err("Unable to activate cow store logical volume.");
		return 0;
	}

	/* FIXME: hard coded dir */
	if (lvm_snprintf(path, sizeof(path), "/dev/device-mapper/%s",
			 buffer) < 0) {
		log_error("Name too long - device not zeroed (%s)",
			  lv->name);
		return 0;
	}

	if (!(dev = dev_cache_get(path, NULL))) {
		log_error("\"%s\" not found: device not zeroed", path);
		return 0;
	}

	if (!(dev_open(dev, O_WRONLY)))
		return 0;

	dev_zero(dev, 0, 4096);
	dev_close(dev);

	return 1;
}


int activate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		count += (!lv_active(lv) && lv_activate(lv));
	}

	return count;
}

int deactivate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		count += ((lv_active(lv) == 1) && lv_deactivate(lv));
	}

	return count;
}

int lvs_in_vg_activated(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		count += (lv_active(lv) == 1);
	}

	return count;
}

int lvs_in_vg_opened(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		count += (lv_open_count(lv) == 1);
	}

	return count;
}
