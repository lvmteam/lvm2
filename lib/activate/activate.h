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
 * Returns 1 if info structure has been populated, else 0.
 */
int lv_info(struct logical_volume *lv, struct dm_info *info);

/*
 * These should eventually use config file
 * to determine whether or not to activate
 */
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s);
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s);
int lv_activate(struct cmd_context *cmd, const char *lvid_s);
int lv_deactivate(struct cmd_context *cmd, const char *lvid_s);

/*
 * FIXME:
 * I don't like the *lvs_in_vg* function names.
 */

/*
 * Return number of LVs in the VG that are active.
 */
int lvs_in_vg_activated(struct volume_group *vg);
int lvs_in_vg_opened(struct volume_group *vg);

int lv_setup_cow_store(struct logical_volume *lv);

#endif
