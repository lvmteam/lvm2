/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
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
#include "filter.h"
#include "segtype.h"

#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#define _skip(fmt, args...) log_very_verbose("Skipping: " fmt , ## args)

int lvm1_present(struct cmd_context *cmd)
{
	char path[PATH_MAX];

	if (lvm_snprintf(path, sizeof(path), "%s/lvm/global", cmd->proc_dir)
	    < 0) {
		log_error("LVM1 proc global snprintf failed");
		return 0;
	}

	if (path_exists(path))
		return 1;
	else
		return 0;
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
int target_present(const char *target_name)
{
	return 0;
}
int lv_info(struct cmd_context *cmd, const struct logical_volume *lv, struct lvinfo *info,
	    int with_open_count)
{
	return 0;
}
int lv_info_by_lvid(struct cmd_context *cmd, const char *lvid_s,
		    struct lvinfo *info, int with_open_count)
{
	return 0;
}
int lv_snapshot_percent(const struct logical_volume *lv, float *percent)
{
	return 0;
}
int lv_mirror_percent(struct cmd_context *cmd, struct logical_volume *lv,
		      int wait, float *percent, uint32_t *event_nr)
{
	return 0;
}
int lvs_in_vg_activated(struct volume_group *vg)
{
	return 0;
}
int lvs_in_vg_opened(struct volume_group *vg)
{
	return 0;
}
int lv_suspend(struct cmd_context *cmd, const char *lvid_s)
{
	return 1;
}
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s)
{
	return 1;
}
int lv_resume(struct cmd_context *cmd, const char *lvid_s)
{
	return 1;
}
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s)
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

int pv_uses_vg(struct cmd_context *cmd, struct physical_volume *pv,
	       struct volume_group *vg)
{
	return 0;
}

void activation_release(void)
{
	return;
}

void activation_exit(void)
{
	return;
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
		log_print("WARNING: Activation disabled. No device-mapper "
			  "interaction will be attempted.");
}

int activation(void)
{
	return _activation;
}

static int _passes_activation_filter(struct cmd_context *cmd,
				     struct logical_volume *lv)
{
	const struct config_node *cn;
	struct config_value *cv;
	char *str;
	char path[PATH_MAX];

	if (!(cn = find_config_tree_node(cmd, "activation/volume_list"))) {
		/* If no host tags defined, activate */
		if (list_empty(&cmd->tags))
			return 1;

		/* If any host tag matches any LV or VG tag, activate */
		if (str_list_match_list(&cmd->tags, &lv->tags) ||
		    str_list_match_list(&cmd->tags, &lv->vg->tags))
			return 1;

		/* Don't activate */
		return 0;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != CFG_STRING) {
			log_error("Ignoring invalid string in config file "
				  "activation/volume_list");
			continue;
		}
		str = cv->v.str;
		if (!*str) {
			log_error("Ignoring empty string in config file "
				  "activation/volume_list");
			continue;
		}

		/* Tag? */
		if (*str == '@') {
			str++;
			if (!*str) {
				log_error("Ignoring empty tag in config file "
					  "activation/volume_list");
				continue;
			}
			/* If any host tag matches any LV or VG tag, activate */
			if (!strcmp(str, "*")) {
				if (str_list_match_list(&cmd->tags, &lv->tags)
				    || str_list_match_list(&cmd->tags,
							   &lv->vg->tags))
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
		if (!index(str, '/')) {
			/* vgname supplied */
			if (!strcmp(str, lv->vg->name))
				return 1;
			else
				continue;
		}
		/* vgname/lvname */
		if (lvm_snprintf(path, sizeof(path), "%s/%s", lv->vg->name,
				 lv->name) < 0) {
			log_error("lvm_snprintf error from %s/%s", lv->vg->name,
				  lv->name);
			continue;
		}
		if (!strcmp(path, str))
			return 1;
	}

	return 0;
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

	if (!dm_task_run(dmt)) {
		log_debug("Failed to get %s target version", target_name);
		/* Assume this was because LIST_VERSIONS isn't supported */
		return 1;
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

		target = (void *) target + target->next;
	} while (last_target != target);

      out:
	dm_task_destroy(dmt);

	return r;
}

int target_present(const char *target_name, int use_modprobe)
{
	uint32_t maj, min, patchlevel;
#ifdef MODPROBE_CMD
	char module[128];
#endif

	if (!activation())
		return 0;

#ifdef MODPROBE_CMD
	if (use_modprobe) {
		if (target_version(target_name, &maj, &min, &patchlevel))
			return 1;

		if (lvm_snprintf(module, sizeof(module), "dm-%s", target_name)
		    < 0) {
			log_error("target_present module name too long: %s",
				  target_name);
			return 0;
		}

		if (!exec_cmd(MODPROBE_CMD, module, "", ""))
			return_0;
	}
#endif

	return target_version(target_name, &maj, &min, &patchlevel);
}

/*
 * Returns 1 if info structure populated, else 0 on failure.
 */
static int _lv_info(struct cmd_context *cmd, const struct logical_volume *lv, int with_mknodes,
		    struct lvinfo *info, int with_open_count)
{
	struct dm_info dminfo;
	char *name;

	if (!activation())
		return 0;

	if (!(name = build_dm_name(cmd->mem, lv->vg->name, lv->name, NULL)))
		return_0;

	log_debug("Getting device info for %s", name);
	if (!dev_manager_info(lv->vg->cmd->mem, name, lv, with_mknodes,
			      with_open_count, &dminfo)) {
		dm_pool_free(cmd->mem, name);
		return_0;
	}

	info->exists = dminfo.exists;
	info->suspended = dminfo.suspended;
	info->open_count = dminfo.open_count;
	info->major = dminfo.major;
	info->minor = dminfo.minor;
	info->read_only = dminfo.read_only;
	info->live_table = dminfo.live_table;
	info->inactive_table = dminfo.inactive_table;

	dm_pool_free(cmd->mem, name);
	return 1;
}

int lv_info(struct cmd_context *cmd, const struct logical_volume *lv, struct lvinfo *info,
	    int with_open_count)
{
	return _lv_info(cmd, lv, 0, info, with_open_count);
}

int lv_info_by_lvid(struct cmd_context *cmd, const char *lvid_s,
		    struct lvinfo *info, int with_open_count)
{
	struct logical_volume *lv;

	if (!(lv = lv_from_lvid(cmd, lvid_s, 0)))
		return 0;

	return _lv_info(cmd, lv, 0, info, with_open_count);
}

/*
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_snapshot_percent(const struct logical_volume *lv, float *percent)
{
	int r;
	struct dev_manager *dm;

	if (!activation())
		return 0;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name)))
		return_0;

	if (!(r = dev_manager_snapshot_percent(dm, lv, percent)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/* FIXME Merge with snapshot_percent */
int lv_mirror_percent(struct cmd_context *cmd, struct logical_volume *lv,
		      int wait, float *percent, uint32_t *event_nr)
{
	int r;
	struct dev_manager *dm;
	struct lvinfo info;

	if (!activation())
		return 0;

	if (!lv_info(cmd, lv, &info, 0))
		return_0;

	if (!info.exists)
		return 0;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name)))
		return_0;

	if (!(r = dev_manager_mirror_percent(dm, lv, wait, percent, event_nr)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

static int _lv_active(struct cmd_context *cmd, struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, &info, 0)) {
		stack;
		return -1;
	}

	return info.exists;
}

static int _lv_open_count(struct cmd_context *cmd, struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, &info, 1)) {
		stack;
		return -1;
	}

	return info.open_count;
}

static int _lv_activate_lv(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name)))
		return_0;

	if (!(r = dev_manager_activate(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_preload(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name)))
		return_0;

	if (!(r = dev_manager_preload(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_deactivate(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name)))
		return_0;

	if (!(r = dev_manager_deactivate(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_suspend_lv(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name)))
		return_0;

	if (!(r = dev_manager_suspend(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

/*
 * These two functions return the number of visible LVs in the state,
 * or -1 on error.
 */
int lvs_in_vg_activated(struct volume_group *vg)
{
	struct lv_list *lvl;
	int count = 0;

	if (!activation())
		return 0;

	list_iterate_items(lvl, &vg->lvs) {
		if (lvl->lv->status & VISIBLE_LV)
			count += (_lv_active(vg->cmd, lvl->lv) == 1);
	}

	return count;
}

int lvs_in_vg_opened(struct volume_group *vg)
{
	struct lv_list *lvl;
	int count = 0;

	if (!activation())
		return 0;

	list_iterate_items(lvl, &vg->lvs) {
		if (lvl->lv->status & VISIBLE_LV)
			count += (_lv_open_count(vg->cmd, lvl->lv) > 0);
	}

	return count;
}

/*
 * register_dev_for_events
 *
 * This function uses proper error codes (but breaks convention)
 * to return:
 *      -1 on error
 *       0 if the lv's targets don't do event [un]registration
 *       0 if the lv is already [un]registered -- FIXME: not implemented
 *       1 if the lv had a segment which was [un]registered
 *
 * Returns: -1 on error
 */
int register_dev_for_events(struct cmd_context *cmd,
			    struct logical_volume *lv, int do_reg)
{
#ifdef DMEVENTD
	int r = 0;
	struct list *tmp;
	struct lv_segment *seg;
	int (*reg) (struct lv_segment *, int events);

	if (do_reg && !dmeventd_register_mode())
		return 1;

	list_iterate(tmp, &lv->segments) {
		seg = list_item(tmp, struct lv_segment);

		reg = NULL;

		if (do_reg) {
			if (seg->segtype->ops->target_register_events)
				reg = seg->segtype->ops->target_register_events;
		} else if (seg->segtype->ops->target_unregister_events)
			reg = seg->segtype->ops->target_unregister_events;

		if (!reg)
			continue;

		/* FIXME specify events */
		if (!reg(seg, 0)) {
			stack;
			return -1;
		}

		r = 1;
	}

	return r;
#else
	return 1;
#endif
}

static int _lv_suspend(struct cmd_context *cmd, const char *lvid_s,
		       int error_if_not_suspended)
{
	struct logical_volume *lv, *lv_pre;
	struct lvinfo info;

	if (!activation())
		return 1;

	if (!(lv = lv_from_lvid(cmd, lvid_s, 0)))
		return_0;

	/* Use precommitted metadata if present */
	if (!(lv_pre = lv_from_lvid(cmd, lvid_s, 1)))
		return_0;

	if (test_mode()) {
		_skip("Suspending '%s'.", lv->name);
		return 1;
	}

	if (!lv_info(cmd, lv, &info, 0))
		return_0;

	if (!info.exists || info.suspended)
		return error_if_not_suspended ? 0 : 1;

	/* If VG was precommitted, preload devices for the LV */
	if ((lv_pre->vg->status & PRECOMMITTED)) {
		if (!_lv_preload(lv_pre)) {
			/* FIXME Revert preloading */
			return_0;
		}
	}

	if (register_dev_for_events(cmd, lv, 0) != 1)
		/* FIXME Consider aborting here */
		stack;

	memlock_inc();
	if (!_lv_suspend_lv(lv)) {
		memlock_dec();
		fs_unlock();
		return 0;
	}

	return 1;
}

/* Returns success if the device is not active */
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s)
{
	return _lv_suspend(cmd, lvid_s, 0);
}

int lv_suspend(struct cmd_context *cmd, const char *lvid_s)
{
	return _lv_suspend(cmd, lvid_s, 1);
}

static int _lv_resume(struct cmd_context *cmd, const char *lvid_s,
		      int error_if_not_active)
{
	struct logical_volume *lv;
	struct lvinfo info;

	if (!activation())
		return 1;

	if (!(lv = lv_from_lvid(cmd, lvid_s, 0)))
		return 0;

	if (test_mode()) {
		_skip("Resuming '%s'.", lv->name);
		return 1;
	}

	if (!lv_info(cmd, lv, &info, 0))
		return_0;

	if (!info.exists || !info.suspended)
		return error_if_not_active ? 0 : 1;

	if (!_lv_activate_lv(lv))
		return 0;

	memlock_dec();
	fs_unlock();

	if (register_dev_for_events(cmd, lv, 1) != 1)
		stack;

	return 1;
}

/* Returns success if the device is not active */
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s)
{
	return _lv_resume(cmd, lvid_s, 0);
}

int lv_resume(struct cmd_context *cmd, const char *lvid_s)
{
	return _lv_resume(cmd, lvid_s, 1);
}

int lv_deactivate(struct cmd_context *cmd, const char *lvid_s)
{
	struct logical_volume *lv;
	struct lvinfo info;
	int r;

	if (!activation())
		return 1;

	if (!(lv = lv_from_lvid(cmd, lvid_s, 0)))
		return 0;

	if (test_mode()) {
		_skip("Deactivating '%s'.", lv->name);
		return 1;
	}

	if (!lv_info(cmd, lv, &info, 1))
		return_0;

	if (!info.exists)
		return 1;

	if (info.open_count && (lv->status & VISIBLE_LV)) {
		log_error("LV %s/%s in use: not deactivating", lv->vg->name,
			  lv->name);
		return 0;
	}

	if (register_dev_for_events(cmd, lv, 0) != 1)
		stack;

	memlock_inc();
	r = _lv_deactivate(lv);
	memlock_dec();
	fs_unlock();

	return r;
}

/* Test if LV passes filter */
int lv_activation_filter(struct cmd_context *cmd, const char *lvid_s,
			 int *activate_lv)
{
	struct logical_volume *lv;

	if (!activation())
		goto activate;

	if (!(lv = lv_from_lvid(cmd, lvid_s, 0)))
		return 0;

	if (!_passes_activation_filter(cmd, lv)) {
		log_verbose("Not activating %s/%s due to config file settings",
			    lv->vg->name, lv->name);
		*activate_lv = 0;
		return 1;
	}

      activate:
	*activate_lv = 1;
	return 1;
}

static int _lv_activate(struct cmd_context *cmd, const char *lvid_s,
			int exclusive, int filter)
{
	struct logical_volume *lv;
	struct lvinfo info;
	int r;

	if (!activation())
		return 1;

	if (!(lv = lv_from_lvid(cmd, lvid_s, 0)))
		return 0;

	if (filter && !_passes_activation_filter(cmd, lv)) {
		log_verbose("Not activating %s/%s due to config file settings",
			    lv->vg->name, lv->name);
		return 0;
	}

	if (test_mode()) {
		_skip("Activating '%s'.", lv->name);
		return 1;
	}

	if (!lv_info(cmd, lv, &info, 0))
		return_0;

	if (info.exists && !info.suspended && info.live_table)
		return 1;

	if (exclusive)
		lv->status |= ACTIVATE_EXCL;

	memlock_inc();
	r = _lv_activate_lv(lv);
	memlock_dec();
	fs_unlock();

	if (!register_dev_for_events(cmd, lv, 1) != 1)
		stack;

	return r;
}

/* Activate LV */
int lv_activate(struct cmd_context *cmd, const char *lvid_s, int exclusive)
{
	return _lv_activate(cmd, lvid_s, exclusive, 0);
}

/* Activate LV only if it passes filter */
int lv_activate_with_filter(struct cmd_context *cmd, const char *lvid_s, int exclusive)
{
	return _lv_activate(cmd, lvid_s, exclusive, 1);
}

int lv_mknodes(struct cmd_context *cmd, const struct logical_volume *lv)
{
	struct lvinfo info;
	int r = 1;

	if (!lv) {
		r = dm_mknodes(NULL);
		fs_unlock();
		return r;
	}

	if (!_lv_info(cmd, lv, 1, &info, 0))
		return_0;

	if (info.exists)
		r = dev_manager_lv_mknodes(lv);
	else
		r = dev_manager_lv_rmnodes(lv);

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
	if (!activation())
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
