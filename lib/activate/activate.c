/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"
#include "log.h"
#include "fs.h"

#include <devmapper/libdevmapper.h>

static void _build_lv_name(char *buffer, size_t s, struct logical_volume *lv)
{
	snprintf(buffer, s, "%s_%s", lv->vg->name, lv->name);
}

static struct dm_task *_setup_task(struct logical_volume *lv, int task)
{
	char name[128];
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task))) {
		stack;
		return NULL;
	}

	_build_lv_name(name, sizeof(name), lv);
	dm_task_set_name(dmt, name);

	return dmt;
}

static struct dm_task *_info(struct logical_volume *lv)
{
	struct dm_task *dmt;

	if (!(dmt = _setup_task(lv, DM_DEVICE_INFO))) {
		stack;
		return 0;
	}

	if (!dm_task_run(dmt)) {
		stack;
		goto bad;
	}

	return dmt;

 bad:
	dm_task_destroy(dmt);
	return NULL;
}

int lv_active(struct logical_volume *lv)
{
	int r = -1;
	struct dm_task *dmt;

	if (!(dmt = _info(lv))) {
		stack;
		return r;
	}

	if (!dm_task_exists(dmt, &r)) {
		stack;
		goto out;
	}

 out:
	dm_task_destroy(dmt);
	return r;
}

int lv_open_count(struct logical_volume *lv)
{
	int r = -1;
	struct dm_task *dmt;

	if (!(dmt = _info(lv))) {
		stack;
		return r;
	}

	if (!dm_task_open_count(dmt, &r)) {
		stack;
		goto out;
	}

 out:
	dm_task_destroy(dmt);
	return r;
}

/*
 * Creates a target for the next contiguous run of
 * extents.
 */
static int _emit_target(struct dm_task *dmt, struct logical_volume *lv,
			unsigned int *ple)
{
	char params[1024];
	unsigned int le = *ple;
	uint64_t esize = lv->vg->extent_size;
	int i, count = 0;
	struct pe_specifier *pes, *first = NULL;

	for (i = le; i < lv->le_count; i++) {
		pes = lv->map + i;

		if (!first)
			first = pes;

		else if (first->pv != pes->pv || first->pe != pes->pe + 1)
				break; /* no longer contig. */

		count++;
	}

	snprintf(params, sizeof(params), "%s %llu",
		 dev_name(first->pv->dev),
		 first->pv->pe_start + (esize * first->pe));

	if (!dm_task_add_target(dmt, esize * le, esize * count,
				"linear", params)) {
		stack;
		return 0;
	}

	*ple = i;
	return 1;
}

int _load(struct logical_volume *lv, int task)
{
	int r = 0;
	uint32_t le = 0;
	struct dm_task *dmt;

	if (!(dmt = _setup_task(lv, task))) {
		stack;
		return 0;
	}

	/*
	 * Merge adjacent extents.
	 */
	while (le < lv->le_count) {
		if (!_emit_target(dmt, lv, &le)) {
			log_error("Unable to activate logical volume '%s'",
				lv->name);
			goto out;
		}
	}

	if (!(r = dm_task_run(dmt)))
		stack;

 out:
	dm_task_destroy(dmt);
	return r;
}

/* FIXME: Always display error msg */
int lv_activate(struct logical_volume *lv)
{
	return _load(lv, DM_DEVICE_CREATE) && fs_add_lv(lv);
}

int _suspend(struct logical_volume *lv, int sus)
{
	int r;
	struct dm_task *dmt;
	int task = sus ? DM_DEVICE_SUSPEND : DM_DEVICE_RESUME;

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

int lv_reactivate(struct logical_volume *lv)
{
	int r;
	if (!_suspend(lv, 1)) {
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
		lv = &(list_item(lvh, struct lv_list)->lv);

		count += (!lv_active(lv) && lv_activate(lv));
	}

	return count;
}

int lv_update_write_access(struct logical_volume *lv)
{
	return 0;
}

int deactivate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = &(list_item(lvh, struct lv_list)->lv);

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
		lv = &(list_item(lvh, struct lv_list)->lv);

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
		lv = &(list_item(lvh, struct lv_list)->lv);

		count += (lv_open_count(lv) == 1);
	}

	return count;
}

