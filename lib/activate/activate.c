/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"

int lv_activate(struct logical_volume *lv)
{
	return 0;
}

int lv_deactivate(struct logical_volume *lv)
{
	return 0;
}

int lvs_in_vg_activated(struct volume_group *vg)
{
	return 0;
}

int lv_update_write_access(struct logical_volume *lv)
{
	return 0;
}

int activate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;

	int done = 0;
	
	list_iterate(lvh, &vg->lvs)
		done += lv_activate(&list_item(lvh, struct lv_list)->lv);

	return done;
}

int deactivate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;

	int done = 0;
	
	list_iterate(lvh, &vg->lvs)
		done += lv_deactivate(&list_item(lvh, struct lv_list)->lv);

	return done;
}


