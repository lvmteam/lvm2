/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef LVM_ACTIVATE_H
#define LVM_ACTIVATE_H

#include "metadata.h"

#ifdef DEVMAPPER_SUPPORT
#  include <libdevmapper.h>
#endif

struct lvinfo {
	int exists;
	int suspended;
	unsigned int open_count;
	int major;
	int minor;
	int read_only;
};

void set_activation(int activation);
int activation(void);

int driver_version(char *version, size_t size);
int library_version(char *version, size_t size);

int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s);
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s);
int lv_activate(struct cmd_context *cmd, const char *lvid_s);
int lv_deactivate(struct cmd_context *cmd, const char *lvid_s);

/*
 * Returns 1 if info structure has been populated, else 0.
 */
int lv_info(const struct logical_volume *lv, struct lvinfo *info);
/*
 * Returns 1 if percent has been set, else 0.
 */
int lv_snapshot_percent(struct logical_volume *lv, float *percent);

/*
 * Return number of LVs in the VG that are active.
 */
int lvs_in_vg_activated(struct volume_group *vg);
int lvs_in_vg_opened(struct volume_group *vg);

int lv_setup_cow_store(struct logical_volume *lv);

#endif
