/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"

int lv_activate(struct volume_group *vg, struct logical_volume *lv)
{
	return 0;
}

int lv_deactivate(struct volume_group *vg, struct logical_volume *lv)
{
	return 0;
}

int lvs_in_vg_activated(struct volume_group *vg)
{
	return 0;
}

int activate_lvs_in_vg(struct volume_group *vg)
{
	return 0;
}

int deactivate_lvs_in_vg(struct volume_group *vg)
{
	return 0;
}


