/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"
#include "display.h"
#include "log.h"
#include "fs.h"
#include "lvm-string.h"
#include "pool.h"
#include "toolcontext.h"
#include "dev_manager.h"

/* FIXME Temporary */
#include "vgcache.h"

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

/*
 * Returns 1 if info structure populated, else 0 on failure.
 */
int lv_info(struct logical_volume *lv, struct dm_info *info)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->name))) {
		stack;
		return 0;
	}

	if (!(r = dev_manager_info(dm, lv, info)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

/*
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_snapshot_percent(struct logical_volume *lv, float *percent)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->name))) {
		stack;
		return 0;
	}

	if (!(r = dev_manager_snapshot_percent(dm, lv, percent)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

static int _lv_active(struct logical_volume *lv)
{
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return -1;
	}

	return info.exists;
}

static int _lv_open_count(struct logical_volume *lv)
{
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return -1;
	}

	return info.open_count;
}

/* FIXME Need to detect and handle an lv rename */
static int _lv_activate(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->name))) {
		stack;
		return 0;
	}

	if (!(r = dev_manager_activate(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_deactivate(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->name))) {
		stack;
		return 0;
	}

	if (!(r = dev_manager_deactivate(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_suspend(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->name))) {
		stack;
		return 0;
	}

	if (!(r = dev_manager_suspend(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

/*
 * These two functions return the number of LVs in the state,
 * or -1 on error.
 */
int lvs_in_vg_activated(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		count += (_lv_active(lv) == 1);
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
		count += (_lv_open_count(lv) == 1);
	}

	return count;
}

static struct logical_volume *_lv_from_lvid(struct cmd_context *cmd,
					    const char *lvid_s)
{
	struct lv_list *lvl;
	struct volume_group *vg;
	union lvid *lvid;

	lvid = (union lvid *) lvid_s;

	log_very_verbose("Finding volume group for uuid %s", lvid_s);
	if (!(vg = vg_read_by_vgid(cmd, lvid->id[0].uuid))) {
		log_error("Volume group for uuid not found: %s", lvid_s);
		return NULL;
	}

	log_verbose("Found volume group \"%s\"", vg->name);
	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg->name);
		return NULL;
	}

	if (!(lvl = find_lv_in_vg_by_lvid(vg, lvid))) {
		log_very_verbose("Can't find logical volume id %s", lvid_s);
		return NULL;
	}

	return lvl->lv;
}

/* These return success if the device is not active */
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s)
{
	struct logical_volume *lv;
	struct dm_info info;

	if (!(lv = _lv_from_lvid(cmd, lvid_s)))
		return 0;

	if (test_mode()) {
		_skip("Suspending '%s'.", lv->name);
		return 0;
	}

	if (!lv_info(lv, &info)) {
		stack;
		return 0;
	}

	if (info.exists && !info.suspended)
		return _lv_suspend(lv);

	return 1;
}

int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s)
{
	struct logical_volume *lv;
	struct dm_info info;

	if (!(lv = _lv_from_lvid(cmd, lvid_s)))
		return 0;

	if (test_mode()) {
		_skip("Resuming '%s'.", lv->name);
		return 0;
	}

	if (!lv_info(lv, &info)) {
		stack;
		return 0;
	}

	if (info.exists && info.suspended)
		return _lv_activate(lv);

	return 1;
}

int lv_deactivate(struct cmd_context *cmd, const char *lvid_s)
{
	struct logical_volume *lv;
	struct dm_info info;

	if (!(lv = _lv_from_lvid(cmd, lvid_s)))
		return 0;

	if (test_mode()) {
		_skip("Deactivating '%s'.", lv->name);
		return 0;
	}

	if (!lv_info(lv, &info)) {
		stack;
		return 0;
	}

	if (info.exists)
		return _lv_deactivate(lv);

	return 1;
}

int lv_activate(struct cmd_context *cmd, const char *lvid_s)
{
	struct logical_volume *lv;
	struct dm_info info;

	if (!(lv = _lv_from_lvid(cmd, lvid_s)))
		return 0;

	if (test_mode()) {
		_skip("Activating '%s'.", lv->name);
		return 0;
	}

	if (!lv_info(lv, &info)) {
		stack;
		return 0;
	}

	if (!info.exists || info.suspended)
		return _lv_activate(lv);

	return 1;
}
