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


int lv_activate(struct logical_volume *lv)
{
	char buffer[128];

	if (test_mode()) {
		_skip("Activation of '%s'.", lv->name);
		return 0;
	}

	if (!build_dm_name(buffer, sizeof(buffer), "",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	if (!device_create_lv(buffer, lv, lv->minor)) {
		stack;
		return 0;
	}

	if (!fs_add_lv(lv, lv->minor)) {
		stack;
		return 0;
	}

	return 1;
}

int lv_reactivate(struct logical_volume *lv)
{
	int r;
	char buffer[128];

	if (test_mode()) {
		_skip("Reactivation of '%s'.", lv->name);
		return 0;
	}

	if (!build_dm_name(buffer, sizeof(buffer), "",
			   lv->vg->name, lv->name)) {
		stack;
		return 0;
	}

	if (!device_suspended(buffer) && !device_suspend(buffer)) {
		stack;
		return 0;
	}

	r = device_reload_lv(buffer, lv);

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

int lv_update_write_access(struct logical_volume *lv)
{
        struct dm_info info;

        if (!lv_info(lv, &info)) {
                stack;
                return 0;
        }

        if (!info.exists || info.suspended)
		/* Noop */
		return 1;

	return lv_reactivate(lv);
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
