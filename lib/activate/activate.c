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
#include "names.h"

#include <limits.h>


int library_version(char *version, size_t size)
{
	if (!dm_get_library_version(version, size))
		return 0;
	return 1;
}

static struct dm_task *_setup_task_with_name(struct logical_volume *lv,
					     const char *lv_name,
				   	     int task)
{
	char name[128];
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task))) {
		stack;
		return NULL;
	}

	if (!build_dm_name(name, sizeof(name), lv->vg->name, lv_name)) {
		stack;
		return NULL;
	}

	dm_task_set_name(dmt, name);

	return dmt;
}

static struct dm_task *_setup_task(struct logical_volume *lv, int task)
{
	return _setup_task_with_name(lv, lv->name, task);
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

int lv_info(struct logical_volume *lv, struct dm_info *info)
{
	int r = 0;
	struct dm_task *dmt;

	log_very_verbose("Getting device info for %s", lv->name);
	if (!(dmt = _setup_task(lv, DM_DEVICE_INFO))) {
		stack;
		return 0;
	}

	if (!dm_task_run(dmt)) {
		stack;
		goto out;
	}

	if (!dm_task_get_info(dmt, info)) {
		stack;
		goto out;
	}
	r = 1;

 out:
	dm_task_destroy(dmt);
	return r;
}

int lv_rename(const char *old_name, struct logical_volume *lv)
{
	int r = 0;
	char new_name[PATH_MAX];
	struct dm_task *dmt;

	if (test_mode())
		return 0;

	if (!(dmt = _setup_task_with_name(lv, old_name, DM_DEVICE_RENAME))) {
		stack;
		return 0;
	}

	if (!build_dm_name(new_name, sizeof(new_name),
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

int lv_active(struct logical_volume *lv)
{
	int r = -1;
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return r;
	}

	log_very_verbose("%s is%s active", lv->name, info.exists ? "":" not");
	return info.exists;
}

int lv_suspended(struct logical_volume *lv)
{
	int r = -1;
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return r;
	}

	log_very_verbose("%s is%s suspended", lv->name,
			 info.suspended ? "":" not");
	return info.suspended;
}

int lv_open_count(struct logical_volume *lv)
{
	int r = -1;
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return r;
	}

	log_very_verbose("%s is open %d time(s)", lv->name, info.open_count);
	return info.open_count;
}

/*
 * Emit a target for a given segment.
 */
static int _emit_target(struct dm_task *dmt, struct stripe_segment *seg)
{
	char params[1024];
	uint64_t esize = seg->lv->vg->extent_size;
	uint32_t s, stripes = seg->stripes;
	int w = 0, tw = 0;
	const char *no_space =
		"Insufficient space to write target parameters.";

	if (stripes > 1) {
		tw = lvm_snprintf(params, sizeof(params), "%u %u ",
			      stripes, seg->stripe_size);

		if (tw < 0) {
			log_err(no_space);
			return 0;
		}

		w = tw;
	}


	for (s = 0; s < stripes; s++, w += tw) {
/******
		log_debug("stripes: %d", stripes);
		log_debug("dev_name(seg->area[s].pv->dev): %s",
				dev_name(seg->area[s].pv->dev));
		log_debug("esize: %" PRIu64, esize);
		log_debug("seg->area[s].pe: %" PRIu64, seg->area[s].pe);
		log_debug("seg->area[s].pv->pe_start: %" PRIu64,
				seg->area[s].pv->pe_start);
*******/

		tw = lvm_snprintf(params + w, sizeof(params) - w,
			      "%s %" PRIu64 "%s",
			      dev_name(seg->area[s].pv->dev),
			      (seg->area[s].pv->pe_start +
			       (esize * seg->area[s].pe)),
			      s == (stripes - 1) ? "" : " ");

		if (tw < 0) {
			log_err(no_space);
			return 0;
		}
	}

	log_debug("Adding target: %" PRIu64 " %" PRIu64 " %s %s",
		   esize * seg->le, esize * seg->len,
		   stripes == 1 ? "linear" : "striped",
		   params);

	if (!dm_task_add_target(dmt, esize * seg->le, esize * seg->len,
				stripes == 1 ? "linear" : "striped",
				params)) {
		stack;
		return 0;
	}

	return 1;
}

int _load(struct logical_volume *lv, int task)
{
	int r = 0;
	struct dm_task *dmt;
	struct list *segh;
	struct stripe_segment *seg;

	log_very_verbose("Generating devmapper parameters for %s", lv->name);
	if (!(dmt = _setup_task(lv, task))) {
		stack;
		return 0;
	}

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct stripe_segment);
		if (!_emit_target(dmt, seg)) {
			log_error("Unable to activate logical volume '%s'",
				lv->name);
			goto out;
		}
	}

	if (!(lv->status & LVM_WRITE) && !dm_task_set_ro(dmt))
		log_error("Failed to set %s read-only during activation.",
			   lv->name);


	if (!(r = dm_task_run(dmt)))
		stack;

	log_verbose("Logical volume %s%s activated", lv->name,
		    r == 1 ? "" : " not");

 out:
	dm_task_destroy(dmt);
	return r;
}

/* FIXME: Always display error msg */
int lv_activate(struct logical_volume *lv)
{
	if (test_mode())
		return 0;

	log_very_verbose("Activating %s", lv->name);
	return _load(lv, DM_DEVICE_CREATE) && fs_add_lv(lv);
}

int _suspend(struct logical_volume *lv, int sus)
{
	int r;
	struct dm_task *dmt;
	int task = sus ? DM_DEVICE_SUSPEND : DM_DEVICE_RESUME;

	log_very_verbose("%s %s", sus ? "Suspending" : "Resuming", lv->name);
	if (!(dmt = _setup_task(lv, task))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_err("Couldn't %s device '%s'", sus ? "suspend" : "resume",
			lv->name);

	dm_task_destroy(dmt);
	return r;
}

int lv_suspend(struct logical_volume *lv)
{
	return _suspend(lv, 1);
}

int lv_reactivate(struct logical_volume *lv)
{
	int r;

	if (test_mode())
		return 0;

	if (!lv_suspended(lv) && !_suspend(lv, 1)) {
		stack;
		return 0;
	}

	r = _load(lv, DM_DEVICE_RELOAD);

	if (!_suspend(lv, 0)) {
		stack;
		return 0;
	}

	return r;
}


int lv_deactivate(struct logical_volume *lv)
{
	int r;
	struct dm_task *dmt;

	log_very_verbose("Deactivating %s", lv->name);
	if (test_mode())
		return 0;

	if (!(dmt = _setup_task(lv, DM_DEVICE_REMOVE))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		stack;

	dm_task_destroy(dmt);

	fs_del_lv(lv);

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
