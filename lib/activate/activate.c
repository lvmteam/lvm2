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

/* FIXME Change these into query macros with lv_get_info() to fill struct? */
int lv_active(struct logical_volume *lv)
{
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return -1;
	}

	return info.exists;
}

int lv_suspended(struct logical_volume *lv)
{
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return -1;
	}

	return info.suspended;
}

int lv_open_count(struct logical_volume *lv)
{
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return -1;
	}

	return info.open_count;
}

int lv_activate(struct logical_volume *lv)
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

/* FIXME Need to detect and handle an lv rename */
int lv_reactivate(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->name))) {
		stack;
		return 0;
	}

	if (!(r = dev_manager_reactivate(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

int lv_deactivate(struct logical_volume *lv)
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

int lv_suspend(struct logical_volume *lv)
{
#if 0
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
#else
	log_err("lv_suspend not implemented.");
	return 1;
#endif
}

int lv_rename(const char *old_name, struct logical_volume *lv)
{
#if 0
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
#else
	log_err("lv_rename not implemented yet.");
	return 1;
#endif
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

/* FIXME Currently lvid is "vgname/lv_uuid". Needs to be vg_uuid/lv_uuid. */
static struct logical_volume *_lv_from_lvid(struct cmd_context *cmd,
					    const char *lvid)
{
	struct lv_list *lvl;
	struct volume_group *vg;
	char *vgname;
	char *slash;

	if (!(slash = strchr(lvid, '/'))) {
		log_error("Invalid VG/LV identifier: %s", lvid);
		return NULL;
	}

	vgname = pool_strdup(cmd->mem, lvid);
	*strchr(vgname, '/') = '\0';

	log_verbose("Finding volume group \"%s\"", vgname);
	if (!(vg = cmd->fid->ops->vg_read(cmd->fid, vgname))) {
		log_error("Volume group \"%s\" doesn't exist", vgname);
		return NULL;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vgname);
		return NULL;
	}

	if (!(lvl = find_lv_in_vg_by_uuid(vg, slash + 1))) {
		log_verbose("Can't find logical volume id %s", lvid);
		return NULL;
	}

	return lvl->lv;
}

/* These functions should become the new interface and the _if_active
 * bits then disappear */
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid)
{
	struct logical_volume *lv;

	if (!(lv = _lv_from_lvid(cmd, lvid)))
		return 0;

	if (lv_active(lv) > 0)
		lv_suspend(lv);

	return 1;
}


int lv_resume_if_active(struct cmd_context *cmd, const char *lvid)
{
	struct logical_volume *lv;

	if (!(lv = _lv_from_lvid(cmd, lvid)))
		return 0;

	if ((lv_active(lv) > 0) && lv_suspended(lv))
		lv_reactivate(lv);

	return 1;
}

int lv_deactivate_if_active(struct cmd_context *cmd, const char *lvid)
{
	struct logical_volume *lv;

	if (!(lv = _lv_from_lvid(cmd, lvid)))
		return 0;

	if (lv_active(lv) > 0)
		lv_deactivate(lv);

	return 1;
}

int lv_activate_if_inactive(struct cmd_context *cmd, const char *lvid)
{
	struct logical_volume *lv;
	int active;

	if (!(lv = _lv_from_lvid(cmd, lvid)))
		return 0;

	active = lv_active(lv);

	if ((active > 0) && lv_suspended(lv)) {
		lv_reactivate(lv);
	} else if (!active)
		lv_activate(lv);

	return 1;
}

