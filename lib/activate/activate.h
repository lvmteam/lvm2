/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef LVM_ACTIVATE_H
#define LVM_ACTIVATE_H

/* FIXME Snapshot handling? */

int lv_activate(struct volume_group *vg,
		struct logical_volume *lv);

int lv_deactivate(struct volume_group *vg,
		  struct logical_volume *lv);

/* Return number of LVs in the VG that are active */
int lvs_in_vg_activated(struct volume_group *vg);

/* Activate all LVs in the VG.  Ignore any that are already active. */
/* Return number activated */
int activate_lvs_in_vg(struct volume_group *vg);

/* Deactivate all LVs in the VG */
int deactivate_lvs_in_vg(struct volume_group *vg);

#endif
