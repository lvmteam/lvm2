/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef LVM_ACTIVATE_H
#define LVM_ACTIVATE_H

#include "metadata-exported.h"

struct lvinfo {
	int exists;
	int suspended;
	unsigned int open_count;
	int major;
	int minor;
	int read_only;
	int live_table;
	int inactive_table;
	uint32_t read_ahead;
};

struct lv_activate_opts {
	int exclusive;
	int origin_only;
	int no_merging;
	int send_messages;
	int skip_in_use;
	unsigned revert;
	unsigned read_only;
};

/* target attribute flags */
#define MIRROR_LOG_CLUSTERED	0x00000001U

/* thin target attribute flags */
enum {
	/* bitfields - new features from 1.1 version */
	THIN_FEATURE_DISCARDS			= (1 << 0),
	THIN_FEATURE_EXTERNAL_ORIGIN		= (1 << 1),
	THIN_FEATURE_HELD_ROOT			= (1 << 2),
	THIN_FEATURE_BLOCK_SIZE			= (1 << 3),
	THIN_FEATURE_DISCARDS_NON_POWER_2	= (1 << 4),
	THIN_FEATURE_METADATA_RESIZE		= (1 << 5),
};

void set_activation(int activation);
int activation(void);

int driver_version(char *version, size_t size);
int library_version(char *version, size_t size);
int lvm1_present(struct cmd_context *cmd);

int module_present(struct cmd_context *cmd, const char *target_name);
int target_present(struct cmd_context *cmd, const char *target_name,
		   int use_modprobe);
int target_version(const char *target_name, uint32_t *maj,
		   uint32_t *min, uint32_t *patchlevel);
int lvm_dm_prefix_check(int major, int minor, const char *prefix);
int list_segment_modules(struct dm_pool *mem, const struct lv_segment *seg,
			 struct dm_list *modules);
int list_lv_modules(struct dm_pool *mem, const struct logical_volume *lv,
		    struct dm_list *modules);

void activation_release(void);
void activation_exit(void);

/* int lv_suspend(struct cmd_context *cmd, const char *lvid_s); */
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only, unsigned exclusive, struct logical_volume *lv_ondisk, struct logical_volume *lv_incore);
int lv_resume(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only, struct logical_volume *lv);
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s,
			unsigned origin_only, unsigned exclusive, unsigned revert, struct logical_volume *lv);
int lv_activate(struct cmd_context *cmd, const char *lvid_s, int exclusive, struct logical_volume *lv);
int lv_activate_with_filter(struct cmd_context *cmd, const char *lvid_s,
			    int exclusive, struct logical_volume *lv);
int lv_deactivate(struct cmd_context *cmd, const char *lvid_s, struct logical_volume *lv);

int lv_mknodes(struct cmd_context *cmd, const struct logical_volume *lv);

/*
 * Returns 1 if info structure has been populated, else 0.
 */
int lv_info(struct cmd_context *cmd, const struct logical_volume *lv, int use_layer,
	    struct lvinfo *info, int with_open_count, int with_read_ahead);
int lv_info_by_lvid(struct cmd_context *cmd, const char *lvid_s, int use_layer,
		    struct lvinfo *info, int with_open_count, int with_read_ahead);

int lv_check_not_in_use(struct cmd_context *cmd, struct logical_volume *lv,
			struct lvinfo *info);

/*
 * Returns 1 if activate_lv has been set: 1 = activate; 0 = don't.
 */
int lv_activation_filter(struct cmd_context *cmd, const char *lvid_s,
			 int *activate_lv, struct logical_volume *lv);
/*
 * Checks against the auto_activation_volume_list and
 * returns 1 if the LV should be activated, 0 otherwise.
 */
int lv_passes_auto_activation_filter(struct cmd_context *cmd, struct logical_volume *lv);

int lv_check_transient(struct logical_volume *lv);
/*
 * Returns 1 if percent has been set, else 0.
 */
int lv_snapshot_percent(const struct logical_volume *lv, percent_t *percent);
int lv_mirror_percent(struct cmd_context *cmd, const struct logical_volume *lv,
		      int wait, percent_t *percent, uint32_t *event_nr);
int lv_raid_percent(const struct logical_volume *lv, percent_t *percent);
int lv_raid_dev_health(const struct logical_volume *lv, char **dev_health);
int lv_raid_mismatch_count(const struct logical_volume *lv, uint64_t *cnt);
int lv_raid_sync_action(const struct logical_volume *lv, char **sync_action);
int lv_raid_message(const struct logical_volume *lv, const char *msg);
int lv_thin_pool_percent(const struct logical_volume *lv, int metadata,
			 percent_t *percent);
int lv_thin_percent(const struct logical_volume *lv, int mapped,
		    percent_t *percent);
int lv_thin_pool_transaction_id(const struct logical_volume *lv,
				uint64_t *transaction_id);

/*
 * Return number of LVs in the VG that are active.
 */
int lvs_in_vg_activated(const struct volume_group *vg);
int lvs_in_vg_opened(const struct volume_group *vg);

int lv_is_active(const struct logical_volume *lv);
int lv_is_active_locally(const struct logical_volume *lv);
int lv_is_active_but_not_locally(const struct logical_volume *lv);
int lv_is_active_exclusive(const struct logical_volume *lv);
int lv_is_active_exclusive_locally(const struct logical_volume *lv);
int lv_is_active_exclusive_remotely(const struct logical_volume *lv);

int lv_has_target_type(struct dm_pool *mem, struct logical_volume *lv,
		       const char *layer, const char *target_type);

int monitor_dev_for_events(struct cmd_context *cmd, struct logical_volume *lv,
			   const struct lv_activate_opts *laopts, int do_reg);

#ifdef DMEVENTD
#  include "libdevmapper-event.h"
char *get_monitor_dso_path(struct cmd_context *cmd, const char *libpath);
int target_registered_with_dmeventd(struct cmd_context *cmd, const char *libpath,
				    struct logical_volume *lv, int *pending);
int target_register_events(struct cmd_context *cmd, const char *dso, struct logical_volume *lv,
			    int evmask __attribute__((unused)), int set, int timeout);
#endif

int add_linear_area_to_dtree(struct dm_tree_node *node, uint64_t size,
			     uint32_t extent_size, int use_linear_target,
			     const char *vgname, const char *lvname);

/*
 * Returns 1 if PV has a dependency tree that uses anything in VG.
 */
int pv_uses_vg(struct physical_volume *pv,
	       struct volume_group *vg);

/*
 * Returns 1 if mapped device is not suspended, blocked or
 * is using a reserved name.
 */
int device_is_usable(struct device *dev);

/*
 * Returns 1 if the device is suspended or blocking.
 * (Does not perform check on the LV name of the device.)
 * N.B.  This is !device_is_usable() without the name check.
 */
int device_is_suspended_or_blocking(struct device *dev);

/*
 * Declaration moved here from fs.h to keep header fs.h hidden
 */
void fs_unlock(void);

#endif
