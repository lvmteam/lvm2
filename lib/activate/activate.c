/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "metadata.h"
#include "activate.h"
#include "memlock.h"
#include "display.h"
#include "fs.h"
#include "lvm-exec.h"
#include "lvm-file.h"
#include "lvm-string.h"
#include "toolcontext.h"
#include "dev_manager.h"
#include "str_list.h"
#include "config.h"
#include "segtype.h"
#include "sharedlib.h"

#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#define _skip(fmt, args...) log_very_verbose("Skipping: " fmt , ## args)

int lvm1_present(struct cmd_context *cmd)
{
	static char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/lvm/global", cmd->proc_dir)
	    < 0) {
		log_error("LVM1 proc global snprintf failed");
		return 0;
	}

	if (path_exists(path))
		return 1;
	else
		return 0;
}

int list_segment_modules(struct dm_pool *mem, const struct lv_segment *seg,
			 struct dm_list *modules)
{
	unsigned int s;
	struct lv_segment *seg2, *snap_seg;
	struct dm_list *snh;

	if (seg->segtype->ops->modules_needed &&
	    !seg->segtype->ops->modules_needed(mem, seg, modules)) {
		log_error("module string allocation failed");
		return 0;
	}

	if (lv_is_origin(seg->lv))
		dm_list_iterate(snh, &seg->lv->snapshot_segs)
			if (!list_lv_modules(mem,
					     dm_list_struct_base(snh,
							      struct lv_segment,
							      origin_list)->cow,
					     modules))
				return_0;

	if (lv_is_cow(seg->lv)) {
		snap_seg = find_snapshot(seg->lv);
		if (snap_seg->segtype->ops->modules_needed &&
		    !snap_seg->segtype->ops->modules_needed(mem, snap_seg,
							    modules)) {
			log_error("snap_seg module string allocation failed");
			return 0;
		}
	}

	for (s = 0; s < seg->area_count; s++) {
		switch (seg_type(seg, s)) {
		case AREA_LV:
			seg2 = find_seg_by_le(seg_lv(seg, s), seg_le(seg, s));
			if (seg2 && !list_segment_modules(mem, seg2, modules))
				return_0;
			break;
		case AREA_PV:
		case AREA_UNASSIGNED:
			;
		}
	}

	return 1;
}

int list_lv_modules(struct dm_pool *mem, const struct logical_volume *lv,
		    struct dm_list *modules)
{
	struct lv_segment *seg;

	dm_list_iterate_items(seg, &lv->segments)
		if (!list_segment_modules(mem, seg, modules))
			return_0;

	return 1;
}

#ifndef DEVMAPPER_SUPPORT
void set_activation(int act)
{
	static int warned = 0;

	if (warned || !act)
		return;

	log_error("Compiled without libdevmapper support. "
		  "Can't enable activation.");

	warned = 1;
}
int activation(void)
{
	return 0;
}
int library_version(char *version, size_t size)
{
	return 0;
}
int driver_version(char *version, size_t size)
{
	return 0;
}
int target_version(const char *target_name, uint32_t *maj,
		   uint32_t *min, uint32_t *patchlevel)
{
	return 0;
}
int target_present(struct cmd_context *cmd, const char *target_name,
		   int use_modprobe)
{
	return 0;
}
int lvm_dm_prefix_check(int major, int minor, const char *prefix)
{
	return 0;
}
int lv_info(struct cmd_context *cmd, const struct logical_volume *lv, int use_layer,
	    struct lvinfo *info, int with_open_count, int with_read_ahead)
{
	return 0;
}
int lv_info_by_lvid(struct cmd_context *cmd, const char *lvid_s, int use_layer,
		    struct lvinfo *info, int with_open_count, int with_read_ahead)
{
	return 0;
}
int lv_check_not_in_use(struct cmd_context *cmd __attribute__((unused)),
			struct logical_volume *lv, struct lvinfo *info)
{
        return 0;
}
int lv_snapshot_percent(const struct logical_volume *lv, percent_t *percent)
{
	return 0;
}
int lv_mirror_percent(struct cmd_context *cmd, const struct logical_volume *lv,
		      int wait, percent_t *percent, uint32_t *event_nr)
{
	return 0;
}
int lv_raid_percent(const struct logical_volume *lv, percent_t *percent)
{
	return 0;
}
int lv_raid_dev_health(const struct logical_volume *lv, char **dev_health)
{
	return 0;
}
int lv_raid_mismatch_count(const struct logical_volume *lv, uint64_t *cnt)
{
	return 0;
}
int lv_raid_sync_action(const struct logical_volume *lv, char **sync_action)
{
	return 0;
}
int lv_raid_message(const struct logical_volume *lv, const char *msg)
{
	return 0;
}
int lv_thin_pool_percent(const struct logical_volume *lv, int metadata,
			 percent_t *percent)
{
	return 0;
}
int lv_thin_percent(const struct logical_volume *lv, int mapped,
		    percent_t *percent)
{
	return 0;
}
int lv_thin_pool_transaction_id(const struct logical_volume *lv,
				uint64_t *transaction_id)
{
	return 0;
}
int lvs_in_vg_activated(const struct volume_group *vg)
{
	return 0;
}
int lvs_in_vg_opened(const struct volume_group *vg)
{
	return 0;
}
/******
int lv_suspend(struct cmd_context *cmd, const char *lvid_s)
{
	return 1;
}
*******/
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only, unsigned exclusive)
{
	return 1;
}
int lv_resume(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only)
{
	return 1;
}
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s,
			unsigned origin_only, unsigned exclusive, unsigned revert)
{
	return 1;
}
int lv_deactivate(struct cmd_context *cmd, const char *lvid_s)
{
	return 1;
}
int lv_activation_filter(struct cmd_context *cmd, const char *lvid_s,
			 int *activate_lv)
{
	return 1;
}
int lv_activate(struct cmd_context *cmd, const char *lvid_s, int exclusive)
{
	return 1;
}
int lv_activate_with_filter(struct cmd_context *cmd, const char *lvid_s, int exclusive)
{
	return 1;
}
int lv_mknodes(struct cmd_context *cmd, const struct logical_volume *lv)
{
	return 1;
}
int pv_uses_vg(struct physical_volume *pv,
	       struct volume_group *vg)
{
	return 0;
}
void activation_release(void)
{
}
void activation_exit(void)
{
}

int lv_is_active(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_locally(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_but_not_locally(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_exclusive(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_exclusive_locally(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_exclusive_remotely(const struct logical_volume *lv)
{
	return 0;
}

int lv_check_transient(struct logical_volume *lv)
{
	return 1;
}
int monitor_dev_for_events(struct cmd_context *cmd, struct logical_volume *lv,
			   const struct lv_activate_opts *laopts, int monitor)
{
	return 1;
}
/* fs.c */
void fs_unlock(void)
{
}
/* dev_manager.c */
#include "targets.h"
int add_areas_line(struct dev_manager *dm, struct lv_segment *seg,
		   struct dm_tree_node *node, uint32_t start_area,
		   uint32_t areas)
{
        return 0;
}
int device_is_usable(struct device *dev)
{
        return 0;
}
int lv_has_target_type(struct dm_pool *mem, struct logical_volume *lv,
		       const char *layer, const char *target_type)
{
        return 0;
}
#else				/* DEVMAPPER_SUPPORT */

static int _activation = 1;

void set_activation(int act)
{
	if (act == _activation)
		return;

	_activation = act;
	if (_activation)
		log_verbose("Activation enabled. Device-mapper kernel "
			    "driver will be used.");
	else
		log_warn("WARNING: Activation disabled. No device-mapper "
			  "interaction will be attempted.");
}

int activation(void)
{
	return _activation;
}

static int _lv_passes_volumes_filter(struct cmd_context *cmd, struct logical_volume *lv,
				     const struct dm_config_node *cn, const int cfg_id)
{
	const struct dm_config_value *cv;
	const char *str;
	static char config_path[PATH_MAX];
	static char path[PATH_MAX];

	config_def_get_path(config_path, sizeof(config_path), cfg_id);
	log_verbose("%s configuration setting defined: "
		    "Checking the list to match %s/%s",
		    config_path, lv->vg->name, lv->name);

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type == DM_CFG_EMPTY_ARRAY)
			goto out;
		if (cv->type != DM_CFG_STRING) {
			log_error("Ignoring invalid string in config file %s",
				  config_path);
			continue;
		}
		str = cv->v.str;
		if (!*str) {
			log_error("Ignoring empty string in config file %s",
				  config_path);
			continue;
		}


		/* Tag? */
		if (*str == '@') {
			str++;
			if (!*str) {
				log_error("Ignoring empty tag in config file "
					  "%s", config_path);
				continue;
			}
			/* If any host tag matches any LV or VG tag, activate */
			if (!strcmp(str, "*")) {
				if (str_list_match_list(&cmd->tags, &lv->tags, NULL)
				    || str_list_match_list(&cmd->tags,
							   &lv->vg->tags, NULL))
					    return 1;
				else
					continue;
			}
			/* If supplied tag matches LV or VG tag, activate */
			if (str_list_match_item(&lv->tags, str) ||
			    str_list_match_item(&lv->vg->tags, str))
				return 1;
			else
				continue;
		}
		if (!strchr(str, '/')) {
			/* vgname supplied */
			if (!strcmp(str, lv->vg->name))
				return 1;
			else
				continue;
		}
		/* vgname/lvname */
		if (dm_snprintf(path, sizeof(path), "%s/%s", lv->vg->name,
				 lv->name) < 0) {
			log_error("dm_snprintf error from %s/%s", lv->vg->name,
				  lv->name);
			continue;
		}
		if (!strcmp(path, str))
			return 1;
	}

out:
	log_verbose("No item supplied in %s configuration setting "
		    "matches %s/%s", config_path, lv->vg->name, lv->name);

	return 0;
}

static int _passes_activation_filter(struct cmd_context *cmd,
				     struct logical_volume *lv)
{
	const struct dm_config_node *cn;

	if (!(cn = find_config_tree_node(cmd, activation_volume_list_CFG, NULL))) {
		log_verbose("activation/volume_list configuration setting "
			    "not defined: Checking only host tags for %s/%s",
			    lv->vg->name, lv->name);

		/* If no host tags defined, activate */
		if (dm_list_empty(&cmd->tags))
			return 1;

		/* If any host tag matches any LV or VG tag, activate */
		if (str_list_match_list(&cmd->tags, &lv->tags, NULL) ||
		    str_list_match_list(&cmd->tags, &lv->vg->tags, NULL))
			return 1;

		log_verbose("No host tag matches %s/%s",
			    lv->vg->name, lv->name);

		/* Don't activate */
		return 0;
	}

	return _lv_passes_volumes_filter(cmd, lv, cn, activation_volume_list_CFG);
}

static int _passes_readonly_filter(struct cmd_context *cmd,
				   struct logical_volume *lv)
{
	const struct dm_config_node *cn;

	if (!(cn = find_config_tree_node(cmd, activation_read_only_volume_list_CFG, NULL)))
		return 0;

	return _lv_passes_volumes_filter(cmd, lv, cn, activation_read_only_volume_list_CFG);
}


int lv_passes_auto_activation_filter(struct cmd_context *cmd, struct logical_volume *lv)
{
	const struct dm_config_node *cn;

	if (!(cn = find_config_tree_node(cmd, activation_auto_activation_volume_list_CFG, NULL))) {
		log_verbose("activation/auto_activation_volume_list configuration setting "
			    "not defined: All logical volumes will be auto-activated.");
		return 1;
	}

	return _lv_passes_volumes_filter(cmd, lv, cn, activation_auto_activation_volume_list_CFG);
}

int library_version(char *version, size_t size)
{
	if (!activation())
		return 0;

	return dm_get_library_version(version, size);
}

int driver_version(char *version, size_t size)
{
	if (!activation())
		return 0;

	log_very_verbose("Getting driver version");

	return dm_driver_version(version, size);
}

int target_version(const char *target_name, uint32_t *maj,
		   uint32_t *min, uint32_t *patchlevel)
{
	int r = 0;
	struct dm_task *dmt;
	struct dm_versions *target, *last_target;

	log_very_verbose("Getting target version for %s", target_name);
	if (!(dmt = dm_task_create(DM_DEVICE_LIST_VERSIONS)))
		return_0;

        if (activation_checks() && !dm_task_enable_checks(dmt))
                goto_out;

	if (!dm_task_run(dmt)) {
		log_debug_activation("Failed to get %s target version", target_name);
		/* Assume this was because LIST_VERSIONS isn't supported */
		*maj = 0;
		*min = 0;
		*patchlevel = 0;
		r = 1;
		goto out;
	}

	target = dm_task_get_versions(dmt);

	do {
		last_target = target;

		if (!strcmp(target_name, target->name)) {
			r = 1;
			*maj = target->version[0];
			*min = target->version[1];
			*patchlevel = target->version[2];
			goto out;
		}

		target = (struct dm_versions *)((char *) target + target->next);
	} while (last_target != target);

      out:
	if (r)
		log_very_verbose("Found %s target "
				 "v%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".",
				 target_name, *maj, *min, *patchlevel);

	dm_task_destroy(dmt);

	return r;
}

int lvm_dm_prefix_check(int major, int minor, const char *prefix)
{
	struct dm_task *dmt;
	const char *uuid;
	int r;

	if (!(dmt = dm_task_create(DM_DEVICE_STATUS)))
		return_0;

	if (!dm_task_set_minor(dmt, minor) ||
	    !dm_task_set_major(dmt, major) ||
	    !dm_task_run(dmt) ||
	    !(uuid = dm_task_get_uuid(dmt))) {
		dm_task_destroy(dmt);
		return 0;
	}

	r = strncasecmp(uuid, prefix, strlen(prefix));
	dm_task_destroy(dmt);

	return r ? 0 : 1;
}

int module_present(struct cmd_context *cmd, const char *target_name)
{
	int ret = 0;
#ifdef MODPROBE_CMD
	char module[128];
	const char *argv[3];

	if (dm_snprintf(module, sizeof(module), "dm-%s", target_name) < 0) {
		log_error("module_present module name too long: %s",
			  target_name);
		return 0;
	}

	argv[0] = MODPROBE_CMD;
	argv[1] = module;
	argv[2] = NULL;

	ret = exec_cmd(cmd, argv, NULL, 0);
#endif
	return ret;
}

int target_present(struct cmd_context *cmd, const char *target_name,
		   int use_modprobe)
{
	uint32_t maj, min, patchlevel;

	if (!activation())
		return 0;

#ifdef MODPROBE_CMD
	if (use_modprobe) {
		if (target_version(target_name, &maj, &min, &patchlevel))
			return 1;

		if (!module_present(cmd, target_name))
			return_0;
	}
#endif

	return target_version(target_name, &maj, &min, &patchlevel);
}

/*
 * Returns 1 if info structure populated, else 0 on failure.
 * When lvinfo* is NULL, it returns 1 if the device is locally active, 0 otherwise.
 */
int lv_info(struct cmd_context *cmd, const struct logical_volume *lv, int use_layer,
	    struct lvinfo *info, int with_open_count, int with_read_ahead)
{
	struct dm_info dminfo;

	if (!activation())
		return 0;
	/*
	 * If open_count info is requested and we have to be sure our own udev
	 * transactions are finished
	 * For non-clustered locking type we are only interested for non-delete operation
	 * in progress - as only those could lead to opened files
	 */
	if (with_open_count) {
		if (locking_is_clustered())
			sync_local_dev_names(cmd); /* Wait to have udev in sync */
		else if (fs_has_non_delete_ops())
			fs_unlock(); /* For non clustered - wait if there are non-delete ops */
	}

	if (!dev_manager_info(lv->vg->cmd->mem, lv,
			      (use_layer) ? lv_layer(lv) : NULL,
			      with_open_count, with_read_ahead,
			      &dminfo, (info) ? &info->read_ahead : NULL))
		return_0;

	if (!info)
		return dminfo.exists;

	info->exists = dminfo.exists;
	info->suspended = dminfo.suspended;
	info->open_count = dminfo.open_count;
	info->major = dminfo.major;
	info->minor = dminfo.minor;
	info->read_only = dminfo.read_only;
	info->live_table = dminfo.live_table;
	info->inactive_table = dminfo.inactive_table;

	return 1;
}

int lv_info_by_lvid(struct cmd_context *cmd, const char *lvid_s, int use_layer,
		    struct lvinfo *info, int with_open_count, int with_read_ahead)
{
	int r;
	struct logical_volume *lv;

	if (!(lv = lv_from_lvid(cmd, lvid_s, 0)))
		return 0;

	r = lv_info(cmd, lv, use_layer, info, with_open_count, with_read_ahead);
	release_vg(lv->vg);

	return r;
}

int lv_check_not_in_use(struct cmd_context *cmd __attribute__((unused)),
			struct logical_volume *lv, struct lvinfo *info)
{
	if (!info->exists)
		return 1;

	/* If sysfs is not used, use open_count information only. */
	if (!*dm_sysfs_dir()) {
		if (info->open_count) {
			log_error("Logical volume %s/%s in use.",
				  lv->vg->name, lv->name);
			return 0;
		}

		return 1;
	}

	if (dm_device_has_holders(info->major, info->minor)) {
		log_error("Logical volume %s/%s is used by another device.",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (dm_device_has_mounted_fs(info->major, info->minor)) {
		log_error("Logical volume %s/%s contains a filesystem in use.",
			  lv->vg->name, lv->name);
		return 0;
	}

	return 1;
}

/*
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_check_transient(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!activation())
		return 0;

	log_debug_activation("Checking transient status for LV %s/%s", lv->vg->name, lv->name);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_transient(dm, lv)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/*
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_snapshot_percent(const struct logical_volume *lv, percent_t *percent)
{
	int r;
	struct dev_manager *dm;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking snapshot percent for LV %s/%s", lv->vg->name, lv->name);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_snapshot_percent(dm, lv, percent)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/* FIXME Merge with snapshot_percent */
int lv_mirror_percent(struct cmd_context *cmd, const struct logical_volume *lv,
		      int wait, percent_t *percent, uint32_t *event_nr)
{
	int r;
	struct dev_manager *dm;

	/* If mirrored LV is temporarily shrinked to 1 area (= linear),
	 * it should be considered in-sync. */
	if (dm_list_size(&lv->segments) == 1 && first_seg(lv)->area_count == 1) {
		*percent = PERCENT_100;
		return 1;
	}

	if (!lv_info(cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking mirror percent for LV %s/%s", lv->vg->name, lv->name);


	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_mirror_percent(dm, lv, wait, percent, event_nr)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

int lv_raid_percent(const struct logical_volume *lv, percent_t *percent)
{
	return lv_mirror_percent(lv->vg->cmd, lv, 0, percent, NULL);
}

int lv_raid_dev_health(const struct logical_volume *lv, char **dev_health)
{
	int r;
	struct dev_manager *dm;
	struct dm_status_raid *status;

	*dev_health = NULL;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking raid device health for LV %s/%s",
			     lv->vg->name, lv->name);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_raid_status(dm, lv, &status)) ||
	    !(*dev_health = dm_pool_strdup(lv->vg->cmd->mem,
					   status->dev_health))) {
		dev_manager_destroy(dm);
		return_0;
	}

	dev_manager_destroy(dm);

	return r;
}

int lv_raid_mismatch_count(const struct logical_volume *lv, uint64_t *cnt)
{
	struct dev_manager *dm;
	struct dm_status_raid *status;

	*cnt = 0;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking raid mismatch count for LV %s/%s",
			     lv->vg->name, lv->name);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!dev_manager_raid_status(dm, lv, &status)) {
		dev_manager_destroy(dm);
		return_0;
	}
	*cnt = status->mismatch_count;

	dev_manager_destroy(dm);

	return 1;
}

int lv_raid_sync_action(const struct logical_volume *lv, char **sync_action)
{
	struct dev_manager *dm;
	struct dm_status_raid *status;
	char *action;

	*sync_action = NULL;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking raid sync_action for LV %s/%s",
			     lv->vg->name, lv->name);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	/* status->sync_action can be NULL if dm-raid version < 1.5.0 */
	if (!dev_manager_raid_status(dm, lv, &status) ||
	    !status->sync_action ||
	    !(action = dm_pool_strdup(lv->vg->cmd->mem,
				      status->sync_action))) {
		dev_manager_destroy(dm);
		return_0;
	}

	*sync_action = action;

	dev_manager_destroy(dm);

	return 1;
}

int lv_raid_message(const struct logical_volume *lv, const char *msg)
{
	int r = 0;
	struct dev_manager *dm;
	struct dm_status_raid *status;

	if (!seg_is_raid(first_seg(lv))) {
		log_error("%s/%s must be a RAID logical volume to"
			  " perform this action.", lv->vg->name, lv->name);
		return 0;
	}

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0)) {
		log_error("Unable to send message to an inactive logical volume.");
		return 0;
	}

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_raid_status(dm, lv, &status))) {
		log_error("Failed to retrieve status of %s/%s",
			  lv->vg->name, lv->name);
		goto out;
	}

	if (!status->sync_action) {
		log_error("Kernel driver does not support this action: %s", msg);
		goto out;
	}

	/*
	 * Note that 'dev_manager_raid_message' allows us to pass down any
	 * currently valid message.  However, this function restricts the
	 * number of user available combinations to a minimum.  Specifically,
	 *     "idle" -> "check"
	 *     "idle" -> "repair"
	 * (The state automatically switches to "idle" when a sync process is
	 * complete.)
	 */
	if (strcmp(msg, "check") && strcmp(msg, "repair")) {
		/*
		 * MD allows "frozen" to operate in a toggling fashion.
		 * We could allow this if we like...
		 */
		log_error("\"%s\" is not a supported sync operation.", msg);
		goto out;
	}
	if (strcmp(status->sync_action, "idle")) {
		log_error("%s/%s state is currently \"%s\".  Unable to switch to \"%s\".",
			  lv->vg->name, lv->name, status->sync_action, msg);
		goto out;
	}

	r = dev_manager_raid_message(dm, lv, msg);
out:
	dev_manager_destroy(dm);

	return r;
}

/*
 * Returns data or metadata percent usage, depends on metadata 0/1.
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_thin_pool_percent(const struct logical_volume *lv, int metadata,
			 percent_t *percent)
{
	int r;
	struct dev_manager *dm;

	if (!activation())
		return 0;

	log_debug_activation("Checking thin %sdata percent for LV %s/%s",
			     (metadata) ? "meta" : "", lv->vg->name, lv->name);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_thin_pool_percent(dm, lv, metadata, percent)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/*
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_thin_percent(const struct logical_volume *lv,
		    int mapped, percent_t *percent)
{
	int r;
	struct dev_manager *dm;

	if (!activation())
		return 0;

	log_debug_activation("Checking thin percent for LV %s/%s",
			     lv->vg->name, lv->name);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_thin_percent(dm, lv, mapped, percent)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/*
 * Returns 1 if transaction_id set, else 0 on failure.
 */
int lv_thin_pool_transaction_id(const struct logical_volume *lv,
				uint64_t *transaction_id)
{
	int r;
	struct dev_manager *dm;
	struct dm_status_thin_pool *status;

	if (!activation())
		return 0;

	log_debug_activation("Checking thin percent for LV %s/%s",
			     lv->vg->name, lv->name);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_thin_pool_status(dm, lv, &status, 1)))
		stack;
	else
		*transaction_id = status->transaction_id;

	dev_manager_destroy(dm);

	return r;
}

static int _lv_active(struct cmd_context *cmd, const struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, 0, &info, 0, 0)) {
		stack;
		return -1;
	}

	return info.exists;
}

static int _lv_open_count(struct cmd_context *cmd, struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, 0, &info, 1, 0)) {
		stack;
		return -1;
	}

	return info.open_count;
}

static int _lv_activate_lv(struct logical_volume *lv, struct lv_activate_opts *laopts)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, (lv->status & PVMOVE) ? 0 : 1)))
		return_0;

	if (!(r = dev_manager_activate(dm, lv, laopts)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_preload(struct logical_volume *lv, struct lv_activate_opts *laopts,
		       int *flush_required)
{
	int r = 0;
	struct dev_manager *dm;
	int old_readonly = laopts->read_only;

	laopts->read_only = _passes_readonly_filter(lv->vg->cmd, lv);

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, (lv->status & PVMOVE) ? 0 : 1)))
		goto_out;

	if (!(r = dev_manager_preload(dm, lv, laopts, flush_required)))
		stack;

	dev_manager_destroy(dm);

	laopts->read_only = old_readonly;
out:
	return r;
}

static int _lv_deactivate(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_deactivate(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_suspend_lv(struct logical_volume *lv, struct lv_activate_opts *laopts,
			  int lockfs, int flush_required)
{
	int r;
	struct dev_manager *dm;

	laopts->read_only = _passes_readonly_filter(lv->vg->cmd, lv);

	/*
	 * When we are asked to manipulate (normally suspend/resume) the PVMOVE
	 * device directly, we don't want to touch the devices that use it.
	 */
	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, (lv->status & PVMOVE) ? 0 : 1)))
		return_0;

	if (!(r = dev_manager_suspend(dm, lv, laopts, lockfs, flush_required)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

/*
 * These two functions return the number of visible LVs in the state,
 * or -1 on error.  FIXME Check this.
 */
int lvs_in_vg_activated(const struct volume_group *vg)
{
	struct lv_list *lvl;
	int count = 0;

	if (!activation())
		return 0;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_is_visible(lvl->lv))
			count += (_lv_active(vg->cmd, lvl->lv) == 1);

	log_debug_activation("Counted %d active LVs in VG %s", count, vg->name);

	return count;
}

int lvs_in_vg_opened(const struct volume_group *vg)
{
	const struct lv_list *lvl;
	int count = 0;

	if (!activation())
		return 0;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_is_visible(lvl->lv))
			count += (_lv_open_count(vg->cmd, lvl->lv) > 0);

	log_debug_activation("Counted %d open LVs in VG %s", count, vg->name);

	return count;
}

/*
 * _lv_is_active
 * @lv:        logical volume being queried
 * @locally:   set if active locally (when provided)
 * @exclusive: set if active exclusively (when provided)
 *
 * Determine whether an LV is active locally or in a cluster.
 * In addition to the return code which indicates whether or
 * not the LV is active somewhere, two other values are set
 * to yield more information about the status of the activation:
 *	return	locally	exclusively	status
 *	======	=======	===========	======
 *	   0	   0	    0		not active
 *	   1	   0	    0		active remotely
 *	   1	   0	    1		exclusive remotely
 *	   1	   1	    0		active locally and possibly remotely
 *	   1	   1	    1		exclusive locally (or local && !cluster)
 * The VG lock must be held to call this function.
 *
 * Returns: 0 or 1
 */
static int _lv_is_active(const struct logical_volume *lv,
			 int *locally, int *exclusive)
{
	int r, l, e; /* remote, local, and exclusive */

	r = l = e = 0;

	if (_lv_active(lv->vg->cmd, lv))
		l = 1;

	if (!vg_is_clustered(lv->vg)) {
		if (l)
			e = 1;  /* exclusive by definition */
		goto out;
	}

	/* Active locally, and the caller doesn't care about exclusive */
	if (l && !exclusive)
		goto out;

	if ((r = remote_lock_held(lv->lvid.s, &e)) >= 0)
		goto out;

	/*
	 * If lock query is not supported (due to interfacing with old
	 * code), then we cannot evaluate exclusivity properly.
	 *
	 * Old users of this function will never be affected by this,
	 * since they are only concerned about active vs. not active.
	 * New users of this function who specifically ask for 'exclusive'
	 * will be given an error message.
	 */
	log_error("Unable to determine exclusivity of %s", lv->name);

	e = 0;

	/*
	 * We used to attempt activate_lv_excl_local(lv->vg->cmd, lv) here,
	 * but it's unreliable.
	 */

out:
	if (locally)
		*locally = l;
	if (exclusive)
		*exclusive = e;

	log_very_verbose("%s/%s is %sactive%s%s",
			 lv->vg->name, lv->name,
			 (r || l) ? "" : "not ",
			 (exclusive && e) ? " exclusive" : "",
			 e ? (l ? " locally" : " remotely") : "");

	return r || l;
}

int lv_is_active(const struct logical_volume *lv)
{
	return _lv_is_active(lv, NULL, NULL);
}

int lv_is_active_locally(const struct logical_volume *lv)
{
	int l;

	return _lv_is_active(lv, &l, NULL) && l;
}

int lv_is_active_but_not_locally(const struct logical_volume *lv)
{
	int l;
	return _lv_is_active(lv, &l, NULL) && !l;
}

int lv_is_active_exclusive(const struct logical_volume *lv)
{
	int e;

	return _lv_is_active(lv, NULL, &e) && e;
}

int lv_is_active_exclusive_locally(const struct logical_volume *lv)
{
	int l, e;

	return _lv_is_active(lv, &l, &e) && l && e;
}

int lv_is_active_exclusive_remotely(const struct logical_volume *lv)
{
	int l, e;

	return _lv_is_active(lv, &l, &e) && !l && e;
}

#ifdef DMEVENTD
static struct dm_event_handler *_create_dm_event_handler(struct cmd_context *cmd, const char *dmuuid, const char *dso,
							 const int timeout, enum dm_event_mask mask)
{
	struct dm_event_handler *dmevh;

	if (!(dmevh = dm_event_handler_create()))
		return_NULL;

	if (dm_event_handler_set_dmeventd_path(dmevh, find_config_tree_str(cmd, dmeventd_executable_CFG, NULL)))
		goto_bad;

	if (dm_event_handler_set_dso(dmevh, dso))
		goto_bad;

	if (dm_event_handler_set_uuid(dmevh, dmuuid))
		goto_bad;

	dm_event_handler_set_timeout(dmevh, timeout);
	dm_event_handler_set_event_mask(dmevh, mask);

	return dmevh;

bad:
	dm_event_handler_destroy(dmevh);
	return NULL;
}

char *get_monitor_dso_path(struct cmd_context *cmd, const char *libpath)
{
	char *path;

	if (!(path = dm_pool_alloc(cmd->mem, PATH_MAX))) {
		log_error("Failed to allocate dmeventd library path.");
		return NULL;
	}

	get_shared_library_path(cmd, libpath, path, PATH_MAX);

	return path;
}

static char *_build_target_uuid(struct cmd_context *cmd, struct logical_volume *lv)
{
	const char *layer;

	if (lv_is_thin_pool(lv))
		layer = "tpool"; /* Monitor "tpool" for the "thin pool". */
	else if (lv_is_origin(lv))
		layer = "real"; /* Monitor "real" for "snapshot-origin". */
	else
		layer = NULL;

	return build_dm_uuid(cmd->mem, lv->lvid.s, layer);
}

int target_registered_with_dmeventd(struct cmd_context *cmd, const char *dso,
				    struct logical_volume *lv, int *pending)
{
	char *uuid;
	enum dm_event_mask evmask = 0;
	struct dm_event_handler *dmevh;
	*pending = 0;

	if (!dso)
		return_0;

	if (!(uuid = _build_target_uuid(cmd, lv)))
		return_0;

	if (!(dmevh = _create_dm_event_handler(cmd, uuid, dso, 0, DM_EVENT_ALL_ERRORS)))
		return_0;

	if (dm_event_get_registered_device(dmevh, 0)) {
		dm_event_handler_destroy(dmevh);
		return 0;
	}

	evmask = dm_event_handler_get_event_mask(dmevh);
	if (evmask & DM_EVENT_REGISTRATION_PENDING) {
		*pending = 1;
		evmask &= ~DM_EVENT_REGISTRATION_PENDING;
	}

	dm_event_handler_destroy(dmevh);

	return evmask;
}

int target_register_events(struct cmd_context *cmd, const char *dso, struct logical_volume *lv,
			    int evmask __attribute__((unused)), int set, int timeout)
{
	char *uuid;
	struct dm_event_handler *dmevh;
	int r;

	if (!dso)
		return_0;

	/* We always monitor the "real" device, never the "snapshot-origin" itself. */
	if (!(uuid = _build_target_uuid(cmd, lv)))
		return_0;

	if (!(dmevh = _create_dm_event_handler(cmd, uuid, dso, timeout,
					       DM_EVENT_ALL_ERRORS | (timeout ? DM_EVENT_TIMEOUT : 0))))
		return_0;

	r = set ? dm_event_register_handler(dmevh) : dm_event_unregister_handler(dmevh);

	dm_event_handler_destroy(dmevh);

	if (!r)
		return_0;

	log_info("%s %s for events", set ? "Monitored" : "Unmonitored", uuid);

	return 1;
}

#endif

/*
 * Returns 0 if an attempt to (un)monitor the device failed.
 * Returns 1 otherwise.
 */
int monitor_dev_for_events(struct cmd_context *cmd, struct logical_volume *lv,
			   const struct lv_activate_opts *laopts, int monitor)
{
#ifdef DMEVENTD
	int i, pending = 0, monitored;
	int r = 1;
	struct dm_list *snh, *snht;
	struct lv_segment *seg;
	struct lv_segment *log_seg;
	int (*monitor_fn) (struct lv_segment *s, int e);
	uint32_t s;
	static const struct lv_activate_opts zlaopts = { 0 };
	struct lvinfo info;

	if (!laopts)
		laopts = &zlaopts;

	/* skip dmeventd code altogether */
	if (dmeventd_monitor_mode() == DMEVENTD_MONITOR_IGNORE)
		return 1;

	/*
	 * Nothing to do if dmeventd configured not to be used.
	 */
	if (monitor && !dmeventd_monitor_mode())
		return 1;

	/*
	 * Allow to unmonitor thin pool via explicit pool unmonitor
	 * or unmonitor before the last thin pool user deactivation
	 * Skip unmonitor, if invoked via deactivation of thin volume
	 * and there is another thin pool user (open_count > 1)
	 * FIXME  think about watch ruler influence.
	 */
	if (laopts->skip_in_use && lv_is_thin_pool(lv) &&
	    lv_info(lv->vg->cmd, lv, 1, &info, 1, 0) && (info.open_count > 1)) {
		log_debug_activation("Skipping unmonitor of opened %s (open:%d)",
				     lv->name, info.open_count);
		return 1;
	}

	/* Do not monitor snapshot that already covers origin */
	if (monitor && lv_is_cow_covering_origin(lv)) {
		log_debug_activation("Skipping monitor of snapshot larger "
				     "then origin %s.", lv->name);
		return 1;
	}

	/*
	 * In case of a snapshot device, we monitor lv->snapshot->lv,
	 * not the actual LV itself.
	 */
	if (lv_is_cow(lv) && (laopts->no_merging || !lv_is_merging_cow(lv)))
		return monitor_dev_for_events(cmd, lv->snapshot->lv, NULL, monitor);

	/*
	 * In case this LV is a snapshot origin, we instead monitor
	 * each of its respective snapshots.  The origin itself may
	 * also need to be monitored if it is a mirror, for example.
	 */
	if (!laopts->origin_only && lv_is_origin(lv))
		dm_list_iterate_safe(snh, snht, &lv->snapshot_segs)
			if (!monitor_dev_for_events(cmd, dm_list_struct_base(snh,
				    struct lv_segment, origin_list)->cow, NULL, monitor))
				r = 0;

	/*
	 * If the volume is mirrored and its log is also mirrored, monitor
	 * the log volume as well.
	 */
	if ((seg = first_seg(lv)) != NULL && seg->log_lv != NULL &&
	    (log_seg = first_seg(seg->log_lv)) != NULL &&
	    seg_is_mirrored(log_seg))
		if (!monitor_dev_for_events(cmd, seg->log_lv, NULL, monitor))
			r = 0;

	dm_list_iterate_items(seg, &lv->segments) {
		/* Recurse for AREA_LV */
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) != AREA_LV)
				continue;
			if (!monitor_dev_for_events(cmd, seg_lv(seg, s), NULL,
						    monitor)) {
				log_error("Failed to %smonitor %s",
					  monitor ? "" : "un",
					  seg_lv(seg, s)->name);
				r = 0;
			}
		}

		/*
		 * If requested unmonitoring of thin volume, preserve skip_in_use flag.
		 *
		 * FIXME: code here looks like _lv_postorder()
		 */
		if (seg->pool_lv &&
		    !monitor_dev_for_events(cmd, seg->pool_lv,
					    (!monitor) ? laopts : NULL, monitor))
			r = 0;

		if (seg->metadata_lv &&
		    !monitor_dev_for_events(cmd, seg->metadata_lv, NULL, monitor))
			r = 0;

		if (!seg_monitored(seg) ||
		    (seg->status & PVMOVE) ||
		    !seg->segtype->ops->target_monitored) /* doesn't support registration */
			continue;

		monitored = seg->segtype->ops->target_monitored(seg, &pending);

		/* FIXME: We should really try again if pending */
		monitored = (pending) ? 0 : monitored;

		monitor_fn = NULL;

		if (monitor) {
			if (monitored)
				log_verbose("%s/%s already monitored.", lv->vg->name, lv->name);
			else if (seg->segtype->ops->target_monitor_events)
				monitor_fn = seg->segtype->ops->target_monitor_events;
		} else {
			if (!monitored)
				log_verbose("%s/%s already not monitored.", lv->vg->name, lv->name);
			else if (seg->segtype->ops->target_unmonitor_events)
				monitor_fn = seg->segtype->ops->target_unmonitor_events;
		}

		/* Do [un]monitor */
		if (!monitor_fn)
			continue;

		log_verbose("%sonitoring %s/%s%s", monitor ? "M" : "Not m", lv->vg->name, lv->name,
			    test_mode() ? " [Test mode: skipping this]" : "");

		/* FIXME Test mode should really continue a bit further. */
		if (test_mode())
			continue;

		/* FIXME specify events */
		if (!monitor_fn(seg, 0)) {
			log_error("%s/%s: %s segment monitoring function failed.",
				  lv->vg->name, lv->name, seg->segtype->name);
			return 0;
		}

		/* Check [un]monitor results */
		/* Try a couple times if pending, but not forever... */
		for (i = 0; i < 10; i++) {
			pending = 0;
			monitored = seg->segtype->ops->target_monitored(seg, &pending);
			if (pending ||
			    (!monitored && monitor) ||
			    (monitored && !monitor))
				log_very_verbose("%s/%s %smonitoring still pending: waiting...",
						 lv->vg->name, lv->name, monitor ? "" : "un");
			else
				break;
			sleep(1);
		}

		if (r)
			r = (monitored && monitor) || (!monitored && !monitor);
	}

	if (!r && !error_message_produced())
		log_error("%sonitoring %s/%s failed.", monitor ? "M" : "Not m",
			  lv->vg->name, lv->name);
	return r;
#else
	return 1;
#endif
}

struct detached_lv_data {
	struct logical_volume *lv_pre;
	struct lv_activate_opts *laopts;
	int *flush_required;
};

static int _preload_detached_lv(struct cmd_context *cmd, struct logical_volume *lv, void *data)
{
	struct detached_lv_data *detached = data;
	struct lv_list *lvl_pre;

	if ((lvl_pre = find_lv_in_vg(detached->lv_pre->vg, lv->name))) {
		if (lv_is_visible(lvl_pre->lv) && lv_is_active(lv) && (!lv_is_cow(lv) || !lv_is_cow(lvl_pre->lv)) &&
		    !_lv_preload(lvl_pre->lv, detached->laopts, detached->flush_required))
			return_0;
	}

	return 1;
}

static int _lv_suspend(struct cmd_context *cmd, const char *lvid_s,
		       struct lv_activate_opts *laopts, int error_if_not_suspended,
	               struct logical_volume *ondisk_lv, struct logical_volume *incore_lv)
{
	struct logical_volume *pvmove_lv = NULL, *ondisk_lv_to_free = NULL, *incore_lv_to_free = NULL;
	struct lv_list *lvl_pre;
	struct seg_list *sl;
        struct lv_segment *snap_seg;
	struct lvinfo info;
	int r = 0, lockfs = 0, flush_required = 0;
	struct detached_lv_data detached;

	if (!activation())
		return 1;

	if (!ondisk_lv && !(ondisk_lv_to_free = ondisk_lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto_out;

	/* Use precommitted metadata if present */
	if (!incore_lv && !(incore_lv_to_free = incore_lv = lv_from_lvid(cmd, lvid_s, 1)))
		goto_out;

	/* Ignore origin_only unless LV is origin in both old and new metadata */
	if (!lv_is_thin_volume(ondisk_lv) && !(lv_is_origin(ondisk_lv) && lv_is_origin(incore_lv)))
		laopts->origin_only = 0;

	if (test_mode()) {
		_skip("Suspending %s%s.", ondisk_lv->name,
		      laopts->origin_only ? " origin without snapshots" : "");
		r = 1;
		goto out;
	}

	if (!lv_info(cmd, ondisk_lv, laopts->origin_only, &info, 0, 0))
		goto_out;

	if (!info.exists || info.suspended) {
		if (!error_if_not_suspended) {
			r = 1;
			if (info.suspended)
				critical_section_inc(cmd, "already suspended");
		}
		goto out;
	}

	if (!lv_read_replicator_vgs(ondisk_lv))
		goto_out;

	lv_calculate_readahead(ondisk_lv, NULL);

	/*
	 * Preload devices for the LV.
	 * If the PVMOVE LV is being removed, it's only present in the old
	 * metadata and not the new, so we must explicitly add the new
	 * tables for all the changed LVs here, as the relationships
	 * are not found by walking the new metadata.
	 */
	if (!(incore_lv->status & LOCKED) &&
	    (ondisk_lv->status & LOCKED) &&
	    (pvmove_lv = find_pvmove_lv_in_lv(ondisk_lv))) {
		/* Preload all the LVs above the PVMOVE LV */
		dm_list_iterate_items(sl, &pvmove_lv->segs_using_this_lv) {
			if (!(lvl_pre = find_lv_in_vg(incore_lv->vg, sl->seg->lv->name))) {
				log_error(INTERNAL_ERROR "LV %s missing from preload metadata", sl->seg->lv->name);
				goto out;
			}
			if (!_lv_preload(lvl_pre->lv, laopts, &flush_required))
				goto_out;
		}
		/* Now preload the PVMOVE LV itself */
		if (!(lvl_pre = find_lv_in_vg(incore_lv->vg, pvmove_lv->name))) {
			log_error(INTERNAL_ERROR "LV %s missing from preload metadata", pvmove_lv->name);
			goto out;
		}
		if (!_lv_preload(lvl_pre->lv, laopts, &flush_required))
			goto_out;
	} else {
		if (!_lv_preload(incore_lv, laopts, &flush_required))
			/* FIXME Revert preloading */
			goto_out;

		/*
		 * Search for existing LVs that have become detached and preload them.
		 */
		detached.lv_pre = incore_lv;
		detached.laopts = laopts;
		detached.flush_required = &flush_required;

		if (!for_each_sub_lv(cmd, ondisk_lv, &_preload_detached_lv, &detached))
			goto_out;

		/*
		 * Preload any snapshots that are being removed.
		 */
		if (!laopts->origin_only && lv_is_origin(ondisk_lv)) {
        		dm_list_iterate_items_gen(snap_seg, &ondisk_lv->snapshot_segs, origin_list) {
				if (!(lvl_pre = find_lv_in_vg_by_lvid(incore_lv->vg, &snap_seg->cow->lvid))) {
					log_error(INTERNAL_ERROR "LV %s (%s) missing from preload metadata",
						  snap_seg->cow->name, snap_seg->cow->lvid.id[1].uuid);
					goto out;
				}
				if (!lv_is_cow(lvl_pre->lv) &&
				    !_lv_preload(lvl_pre->lv, laopts, &flush_required))
					goto_out;
			}
		}
	}

	if (!monitor_dev_for_events(cmd, ondisk_lv, laopts, 0))
		/* FIXME Consider aborting here */
		stack;

	critical_section_inc(cmd, "suspending");
	if (pvmove_lv)
		critical_section_inc(cmd, "suspending pvmove LV");

	if (!laopts->origin_only &&
	    (lv_is_origin(incore_lv) || lv_is_cow(incore_lv)))
		lockfs = 1;

	/* Converting non-thin LV to thin external origin ? */
	if (!lv_is_thin_volume(ondisk_lv) && lv_is_thin_volume(incore_lv))
		lockfs = 1; /* Sync before conversion */

	if (laopts->origin_only && lv_is_thin_volume(ondisk_lv) && lv_is_thin_volume(incore_lv))
		lockfs = 1;

	/*
	 * Suspending an LV directly above a PVMOVE LV also
 	 * suspends other LVs using that same PVMOVE LV.
	 * FIXME Remove this and delay the 'clear node' until
 	 * after the code knows whether there's a different
 	 * inactive table to load or not instead so lv_suspend
 	 * can be called separately for each LV safely.
 	 */
	if ((incore_lv->vg->status & PRECOMMITTED) &&
	    (incore_lv->status & LOCKED) && find_pvmove_lv_in_lv(incore_lv)) {
		if (!_lv_suspend_lv(incore_lv, laopts, lockfs, flush_required)) {
			critical_section_dec(cmd, "failed precommitted suspend");
			if (pvmove_lv)
				critical_section_dec(cmd, "failed precommitted suspend (pvmove)");
			goto_out;
		}
	} else {
		/* Normal suspend */
		if (!_lv_suspend_lv(ondisk_lv, laopts, lockfs, flush_required)) {
			critical_section_dec(cmd, "failed suspend");
			if (pvmove_lv)
				critical_section_dec(cmd, "failed suspend (pvmove)");
			goto_out;
		}
	}

	r = 1;
out:
	if (incore_lv_to_free)
		release_vg(incore_lv_to_free->vg);
	if (ondisk_lv_to_free) {
		lv_release_replicator_vgs(ondisk_lv_to_free);
		release_vg(ondisk_lv_to_free->vg);
	}

	return r;
}

/*
 * In a cluster, set exclusive to indicate that only one node is using the
 * device.  Any preloaded tables may then use non-clustered targets.
 *
 * Returns success if the device is not active
 */
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only, unsigned exclusive, struct logical_volume *ondisk_lv, struct logical_volume *incore_lv)
{
	struct lv_activate_opts laopts = {
		.origin_only = origin_only,
		.exclusive = exclusive
	};

	return _lv_suspend(cmd, lvid_s, &laopts, 0, ondisk_lv, incore_lv);
}

/* No longer used */
/***********
int lv_suspend(struct cmd_context *cmd, const char *lvid_s)
{
	return _lv_suspend(cmd, lvid_s, 1);
}
***********/

static int _lv_resume(struct cmd_context *cmd, const char *lvid_s,
		      struct lv_activate_opts *laopts, int error_if_not_active,
	              struct logical_volume *lv)
{
	struct logical_volume *lv_to_free = NULL;
	struct lvinfo info;
	int r = 0;
	int messages_only = 0;

	if (!activation())
		return 1;

	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto_out;

	if (lv_is_thin_pool(lv) && laopts->origin_only)
		messages_only = 1;

	if (!lv_is_origin(lv) && !lv_is_thin_volume(lv))
		laopts->origin_only = 0;

	if (test_mode()) {
		_skip("Resuming %s%s%s.", lv->name, laopts->origin_only ? " without snapshots" : "",
		      laopts->revert ? " (reverting)" : "");
		r = 1;
		goto out;
	}

	log_debug_activation("Resuming LV %s/%s%s%s%s.", lv->vg->name, lv->name,
			     error_if_not_active ? "" : " if active",
			     laopts->origin_only ? " without snapshots" : "",
			     laopts->revert ? " (reverting)" : "");

	if (!lv_info(cmd, lv, laopts->origin_only, &info, 0, 0))
		goto_out;

	if (!info.exists || !(info.suspended || messages_only)) {
		if (error_if_not_active)
			goto_out;
		r = 1;
		if (!info.suspended)
			critical_section_dec(cmd, "already resumed");
		goto out;
	}

	laopts->read_only = _passes_readonly_filter(cmd, lv);

	if (!_lv_activate_lv(lv, laopts))
		goto_out;

	critical_section_dec(cmd, "resumed");

	if (!monitor_dev_for_events(cmd, lv, laopts, 1))
		stack;

	r = 1;
out:
	if (lv_to_free)
		release_vg(lv_to_free->vg);

	return r;
}

/*
 * In a cluster, set exclusive to indicate that only one node is using the
 * device.  Any tables loaded may then use non-clustered targets.
 *
 * @origin_only
 * @exclusive   This parameter only has an affect in cluster-context.
 *              It forces local target type to be used (instead of
 *              cluster-aware type).
 * Returns success if the device is not active
 */
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s,
			unsigned origin_only, unsigned exclusive,
			unsigned revert, struct logical_volume *lv)
{
	struct lv_activate_opts laopts = {
		.origin_only = origin_only,
		.exclusive = exclusive,
		.revert = revert
	};

	return _lv_resume(cmd, lvid_s, &laopts, 0, lv);
}

int lv_resume(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only, struct logical_volume *lv)
{
	struct lv_activate_opts laopts = { .origin_only = origin_only, };

	return _lv_resume(cmd, lvid_s, &laopts, 1, lv);
}

static int _lv_has_open_snapshots(struct logical_volume *lv)
{
	struct lv_segment *snap_seg;
	struct lvinfo info;
	int r = 0;

	dm_list_iterate_items_gen(snap_seg, &lv->snapshot_segs, origin_list) {
		if (!lv_info(lv->vg->cmd, snap_seg->cow, 0, &info, 1, 0)) {
			r = 1;
			continue;
		}

		if (info.exists && info.open_count) {
			log_error("LV %s/%s has open snapshot %s: "
				  "not deactivating", lv->vg->name, lv->name,
				  snap_seg->cow->name);
			r = 1;
		}
	}

	return r;
}

int lv_deactivate(struct cmd_context *cmd, const char *lvid_s, struct logical_volume *lv)
{
	struct logical_volume *lv_to_free = NULL;
	struct lvinfo info;
	static const struct lv_activate_opts laopts = { .skip_in_use = 1 };
	int r = 0;

	if (!activation())
		return 1;

	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto out;

	if (test_mode()) {
		_skip("Deactivating '%s'.", lv->name);
		r = 1;
		goto out;
	}

	log_debug_activation("Deactivating %s/%s.", lv->vg->name, lv->name);

	if (!lv_info(cmd, lv, 0, &info, 1, 0))
		goto_out;

	if (!info.exists) {
		r = 1;
		goto out;
	}

	if (lv_is_visible(lv)) {
		if (!lv_check_not_in_use(cmd, lv, &info))
			goto_out;

		if (lv_is_origin(lv) && _lv_has_open_snapshots(lv))
			goto_out;
	}

	if (!lv_read_replicator_vgs(lv))
		goto_out;

	if (!monitor_dev_for_events(cmd, lv, &laopts, 0))
		stack;

	critical_section_inc(cmd, "deactivating");
	r = _lv_deactivate(lv);
	critical_section_dec(cmd, "deactivated");

	if (!lv_info(cmd, lv, 0, &info, 0, 0) || info.exists)
		r = 0;
out:
	if (lv_to_free) {
		lv_release_replicator_vgs(lv_to_free);
		release_vg(lv_to_free->vg);
	}

	return r;
}

/* Test if LV passes filter */
int lv_activation_filter(struct cmd_context *cmd, const char *lvid_s,
			 int *activate_lv, struct logical_volume *lv)
{
	struct logical_volume *lv_to_free = NULL;
	int r = 0;

	if (!activation()) {
		*activate_lv = 1;
		return 1;
	}

	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto out;

	if (!_passes_activation_filter(cmd, lv)) {
		log_verbose("Not activating %s/%s since it does not pass "
			    "activation filter.", lv->vg->name, lv->name);
		*activate_lv = 0;
	} else
		*activate_lv = 1;
	r = 1;
out:
	if (lv_to_free)
		release_vg(lv_to_free->vg);

	return r;
}

static int _lv_activate(struct cmd_context *cmd, const char *lvid_s,
			struct lv_activate_opts *laopts, int filter,
	                struct logical_volume *lv)
{
	struct logical_volume *lv_to_free = NULL;
	struct lvinfo info;
	int r = 0;

	if (!activation())
		return 1;

	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto out;

	if (filter && !_passes_activation_filter(cmd, lv)) {
		log_error("Not activating %s/%s since it does not pass "
			  "activation filter.", lv->vg->name, lv->name);
		goto out;
	}

	if ((!lv->vg->cmd->partial_activation) && (lv->status & PARTIAL_LV)) {
		log_error("Refusing activation of partial LV %s. Use --partial to override.",
			  lv->name);
		goto out;
	}

	if (lv_has_unknown_segments(lv)) {
		log_error("Refusing activation of LV %s containing "
			  "an unrecognised segment.", lv->name);
		goto out;
	}

	if (test_mode()) {
		_skip("Activating '%s'.", lv->name);
		r = 1;
		goto out;
	}

	if (filter)
		laopts->read_only = _passes_readonly_filter(cmd, lv);

	log_debug_activation("Activating %s/%s%s%s.", lv->vg->name, lv->name,
			     laopts->exclusive ? " exclusively" : "",
			     laopts->read_only ? " read-only" : "");

	if (!lv_info(cmd, lv, 0, &info, 0, 0))
		goto_out;

	/*
	 * Nothing to do?
	 */
	if (info.exists && !info.suspended && info.live_table &&
	    (info.read_only == read_only_lv(lv, laopts))) {
		r = 1;
		goto out;
	}

	if (!lv_read_replicator_vgs(lv))
		goto_out;

	lv_calculate_readahead(lv, NULL);

	critical_section_inc(cmd, "activating");
	if (!(r = _lv_activate_lv(lv, laopts)))
		stack;
	critical_section_dec(cmd, "activated");

	if (r && !monitor_dev_for_events(cmd, lv, laopts, 1))
		stack;

out:
	if (lv_to_free) {
		lv_release_replicator_vgs(lv_to_free);
		release_vg(lv_to_free->vg);
	}

	return r;
}

/* Activate LV */
int lv_activate(struct cmd_context *cmd, const char *lvid_s, int exclusive, struct logical_volume *lv)
{
	struct lv_activate_opts laopts = { .exclusive = exclusive };

	if (!_lv_activate(cmd, lvid_s, &laopts, 0, lv))
		return_0;

	return 1;
}

/* Activate LV only if it passes filter */
int lv_activate_with_filter(struct cmd_context *cmd, const char *lvid_s, int exclusive, struct logical_volume *lv)
{
	struct lv_activate_opts laopts = { .exclusive = exclusive };

	if (!_lv_activate(cmd, lvid_s, &laopts, 1, lv))
		return_0;

	return 1;
}

int lv_mknodes(struct cmd_context *cmd, const struct logical_volume *lv)
{
	int r = 1;

	if (!lv) {
		r = dm_mknodes(NULL);
		fs_unlock();
		return r;
	}

	if (!activation())
		return 1;

	r = dev_manager_mknodes(lv);

	fs_unlock();

	return r;
}

/*
 * Does PV use VG somewhere in its construction?
 * Returns 1 on failure.
 */
int pv_uses_vg(struct physical_volume *pv,
	       struct volume_group *vg)
{
	if (!activation() || !pv->dev)
		return 0;

	if (!dm_is_dm_major(MAJOR(pv->dev->dev)))
		return 0;

	return dev_manager_device_uses_vg(pv->dev, vg);
}

void activation_release(void)
{
	dev_manager_release();
}

void activation_exit(void)
{
	dev_manager_exit();
}
#endif
