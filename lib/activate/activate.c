/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"
#include "log.h"

#include <devmapper/libdevmapper.h>

int lv_activate(struct logical_volume *lv)
{
	int r = 0;
	int i;

	uint64_t esize = lv->vg->extent_size;
	uint64_t start = 0ull;
	char params[1024];
	struct pe_specifier *pes;
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_CREATE))) {
		stack;
		return 0;
	}

	dm_task_set_name(dmt, lv->id.uuid);

	for (i = 0; i < lv->le_count; i++) {
		pes = lv->map + i;
		snprintf(params, sizeof(params), "%s %llu",
			 dev_name(pes->pv->dev),
			 pes->pv->pe_start + (esize * pes->pe));

		if (!dm_task_add_target(dmt, start, esize, "linear", params)) {
			stack;
			goto out;
		}

		start += esize;
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
	struct dm_task *dmt = dm_task_create(DM_DEVICE_REMOVE);
	if (!dmt) {
		stack;
		return 0;
	}

	dm_task_set_name(dmt, lv->id.uuid);
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
