/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"
#include "log.h"

#include <devmapper/libdevmapper.h>

static void _build_lv_name(char *buffer, size_t s, struct logical_volume *lv)
{
	snprintf(buffer, s, "%s_%s", lv->vg->name, lv->name);
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

int lv_activate(struct logical_volume *lv)
{
	int r = 0;
	uint32_t le = 0;

	char name[128];
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_CREATE))) {
		stack;
		return 0;
	}

	_build_lv_name(name, sizeof(name), lv);
	dm_task_set_name(dmt, name);

	/*
	 * Merge adjacent extents.
	 */
	while (le < lv->le_count) {
		if (!_emit_target(dmt, lv, &le)) {
			log_err("unable to activate logical volume '%s'",
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

int activate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;

	int done = 0;

	list_iterate(lvh, &vg->lvs)
		done += lv_activate(&list_item(lvh, struct lv_list)->lv);

	return done;
}

int lv_deactivate(struct logical_volume *lv)
{
	int r;
	char name[128];
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_REMOVE))) {
		stack;
		return 0;
	}

	_build_lv_name(name, sizeof(name), lv);
	dm_task_set_name(dmt, name);

	if (!(r = dm_task_run(dmt)))
		stack;

	dm_task_destroy(dmt);
	return r;
}

int lv_update_write_access(struct logical_volume *lv)
{
	return 0;
}

int deactivate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;

	int done = 0;

	list_iterate(lvh, &vg->lvs)
		done += lv_deactivate(&list_item(lvh, struct lv_list)->lv);

	return done;
}

int lvs_in_vg_activated(struct volume_group *vg)
{
	return 0;
}
