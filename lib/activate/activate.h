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
 * Status functions.  Return count (0 upwards) or else -1 on
 * error.
 */
int lv_active(struct logical_volume *lv);
int lv_suspended(struct logical_volume *lv);
int lv_open_count(struct logical_volume *lv);

/*
 * Returns 1 if info structure has been populated, else 0.
 */
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
 * These should eventually replace some of the above and maybe
 * use config file to determine whether or not to activate
 */
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid);
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid);


/*
 * FIXME:
 * I don't like the *lvs_in_vg* function names.
 */

/*
 * Return number of LVs in the VG that are active.
 */
int lvs_in_vg_activated(struct volume_group *vg);
int lvs_in_vg_opened(struct volume_group *vg);

/*
 * Activate all LVs in the VG.  Ignore any that are already
 * active.  Return number actually activated.
 */
int activate_lvs_in_vg(struct volume_group *vg);

/*
 * Deactivate all LVs in the VG.  Return number actually deactivated.
 */
int deactivate_lvs_in_vg(struct volume_group *vg);



int lv_setup_cow_store(struct logical_volume *lv);

#endif
