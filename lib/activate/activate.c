/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"

#include <device-mapper/libdm.h>

int lv_activate(struct logical_volume *lv)
{
	int r = 0;
	uint64_t esize = lv->vg->extent_size;
	uint64_t start = 0ull;
	char params[1024];
	struct pe_specifier *pes;
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_CREATE))) {
		stack;
		return 0;
	}

	dm_command_set_name(c, lv->uuid);

	for (i = 0; i < lv->le_count; i++) {
		pes = lv->map + i;
		snprintf(params, sizeof(params), "%s %ull",
			 dev_name(pes->pv.dev),
			 pes->pv->pe_start + (esize * pes->pe));

		if (!dm_task_add_target(dmt, start, size, "linear", params)) {
			stack;
			goto out;
		}

		start += esize;
	}

	if (!(r = dm_task_run(c)))
		stack;

 out:
	dm_command_destroy(c);
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
	struct dm_task *dmt = dm_command_create(DM_DEVICE_REMOVE);
	if (!c) {
		stack;
		return 0;
	}

	dm_command_set_name(c, lv->uuid);
	if (!(r = dm_command_run()))
		stack;

 out:
	dm_task_destroy(dmt);
	return r;
}

int lv_deactivate(struct volume_group *vg, struct logical_volume *lv)
{
	return 0;
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
