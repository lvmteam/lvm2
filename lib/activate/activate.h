/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef LVM_ACTIVATE_H
#define LVM_ACTIVATE_H

#include <libdevmapper.h>

int driver_version(char *version, size_t size);
int library_version(char *version, size_t size);

/*
 * Status functions.
 */
int lv_active(struct logical_volume *lv);
int lv_suspended(struct logical_volume *lv);
int lv_open_count(struct logical_volume *lv);
int lv_info(struct logical_volume *lv, struct dm_info *info);

/*
 * Activation proper.
 */
int lv_activate(struct logical_volume *lv);
int lv_reactivate(struct logical_volume *lv);
int lv_deactivate(struct logical_volume *lv);
int lv_suspend(struct logical_volume *lv);
int lv_rename(const char *old_name, struct logical_volume *lv);

/*
 * Return number of LVs in the VG that are active.
 */
int lvs_in_vg_activated(struct volume_group *vg);
int lvs_in_vg_opened(struct volume_group *vg);

/*
 * Test for (lv->status & LVM_WRITE)
 */
int lv_update_write_access(struct logical_volume *lv);

/*
 * Activate all LVs in the VG.  Ignore any that are already
 * active.  Return number activated.
 */
int activate_lvs_in_vg(struct volume_group *vg);

/*
 * Deactivate all LVs in the VG
 */
int deactivate_lvs_in_vg(struct volume_group *vg);

#endif
