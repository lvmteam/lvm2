/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2016 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"

#include "memlock.h"

static int _lvchange_permission(struct cmd_context *cmd,
				struct logical_volume *lv)
{
	uint32_t lv_access;
	struct lvinfo info;

	lv_access = arg_uint_value(cmd, permission_ARG, 0);

	if (lv_is_external_origin(lv)) {
		log_error("Cannot change permissions of external origin %s.",
			  display_lvname(lv));
		return 0;
	}

	if (!(lv_access & LVM_WRITE) && !(lv->status & LVM_WRITE)) {
		/* Refresh if it's read-only in metadata but read-write in kernel */
		if (lv_info(cmd, lv, 0, &info, 0, 0) && info.exists && !info.read_only) {
			log_print_unless_silent("Logical volume %s is already read-only.  Refreshing kernel state.",
						display_lvname(lv));
			return lv_refresh(cmd, lv);
		}
		log_error("Logical volume \"%s\" is already read only.",
			  display_lvname(lv));
		return 0;
	}

	if ((lv_access & LVM_WRITE) && (lv->status & LVM_WRITE)) {
		/* Refresh if it's read-write in metadata but read-only in kernel */
		if (lv_info(cmd, lv, 0, &info, 0, 0) && info.exists && info.read_only) {
			log_print_unless_silent("Logical volume %s is already writable.  Refreshing kernel state.",
						display_lvname(lv));
			return lv_refresh(cmd, lv);
		}

		log_error("Logical volume %s is already writable.",
			  display_lvname(lv));
		return 0;
	}

	if (lv_is_mirrored(lv) && vg_is_clustered(lv->vg) &&
	    lv_info(cmd, lv, 0, &info, 0, 0) && info.exists) {
		log_error("Cannot change permissions of mirror %s while active.",
			  display_lvname(lv));
		return 0;
	}

	/* Not allowed to change permissions on RAID sub-LVs directly */
	if (lv_is_raid_metadata(lv) || lv_is_raid_image(lv)) {
		log_error("Cannot change permissions of RAID %s %s.",
			  lv_is_raid_image(lv) ? "image" : "metadata area",
			  display_lvname(lv));
		return 0;
	}

	if (!(lv_access & LVM_WRITE) && lv_is_thin_pool(lv)) {
		log_error("Change permissions of thin pool %s not yet supported.",
			  display_lvname(lv));
		return 0;
	}

	if (lv_access & LVM_WRITE) {
		lv->status |= LVM_WRITE;
		log_verbose("Setting logical volume %s read/write.",
			    display_lvname(lv));
	} else {
		lv->status &= ~LVM_WRITE;
		log_verbose("Setting logical volume %s read-only.",
			    display_lvname(lv));
	}

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

static int _lvchange_pool_update(struct cmd_context *cmd,
				 struct logical_volume *lv)
{
	int update = 0;
	unsigned val;
	thin_discards_t discards;

	if (!lv_is_thin_pool(lv)) {
		log_error("Logical volume %s is not a thin pool.",
			  display_lvname(lv));
		return 0;
	}

	if (arg_is_set(cmd, discards_ARG)) {
		discards = (thin_discards_t) arg_uint_value(cmd, discards_ARG, THIN_DISCARDS_IGNORE);
		if (discards != first_seg(lv)->discards) {
			if (((discards == THIN_DISCARDS_IGNORE) ||
			     (first_seg(lv)->discards == THIN_DISCARDS_IGNORE)) &&
			    pool_is_active(lv))
				log_error("Cannot change support for discards while pool volume %s is active.",
					  display_lvname(lv));
			else {
				first_seg(lv)->discards = discards;
				update++;
			}
		} else
			log_error("Logical volume %s already uses --discards %s.",
				  display_lvname(lv), get_pool_discards_name(discards));
	}

	if (arg_is_set(cmd, zero_ARG)) {
		val = arg_uint_value(cmd, zero_ARG, 1);
		if (val != first_seg(lv)->zero_new_blocks) {
			first_seg(lv)->zero_new_blocks = val;
			update++;
		} else
			log_error("Logical volume %s already %szero new blocks.",
				  display_lvname(lv), val ? "" : "does not ");
	}

	if (!update)
		return 0;

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

static int _lvchange_monitoring(struct cmd_context *cmd,
				struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, lv_is_thin_pool(lv) ? 1 : 0,
		     &info, 0, 0) || !info.exists) {
		log_error("Logical volume %s is not active.", display_lvname(lv));
		return 0;
	}

	/* do not monitor pvmove lv's */
	if (lv_is_pvmove(lv))
		return 1;

	if ((dmeventd_monitor_mode() != DMEVENTD_MONITOR_IGNORE) &&
	    !monitor_dev_for_events(cmd, lv, 0, dmeventd_monitor_mode()))
		return_0;

	return 1;
}

static int _lvchange_background_polling(struct cmd_context *cmd,
					struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, 0, &info, 0, 0) || !info.exists) {
		log_error("Logical volume %s is not active.", display_lvname(lv));
		return 0;
	}

	if (background_polling())
		lv_spawn_background_polling(cmd, lv);

	return 1;
}

static int _lvchange_activate(struct cmd_context *cmd, struct logical_volume *lv)
{
	activation_change_t activate;

	activate = (activation_change_t) arg_uint_value(cmd, activate_ARG, CHANGE_AY);

	/*
	 * We can get here in the odd case where an LV is already active in
	 * a foreign VG, which allows the VG to be accessed by lvchange -a
	 * so the LV can be deactivated.
	 */
	if (lv->vg->system_id && lv->vg->system_id[0] &&
	    cmd->system_id && cmd->system_id[0] &&
	    strcmp(lv->vg->system_id, cmd->system_id) &&
	    is_change_activating(activate)) {
		log_error("Cannot activate LVs in a foreign VG.");
		return ECMD_FAILED;
	}

	if (lv_activation_skip(lv, activate, arg_is_set(cmd, ignoreactivationskip_ARG)))
		return 1;

	if (lv_is_cow(lv) && !lv_is_virtual_origin(origin_from_cow(lv)))
		lv = origin_from_cow(lv);

	if ((activate == CHANGE_AAY) &&
	    !lv_passes_auto_activation_filter(cmd, lv))
		return 1;

	if (!lv_change_activate(cmd, lv, activate))
		return_0;

	/*
	 * FIXME: lvchange should defer background polling in a similar
	 * 	  way as vgchange does. First activate all relevant LVs
	 * 	  initate background polling later (for all actually
	 * 	  activated LVs). So we can avoid duplicate background
	 * 	  polling for pvmove (2 or more locked LVs on single pvmove
	 * 	  LV)
	 */
	if (background_polling() && is_change_activating(activate) &&
	    (lv_is_pvmove(lv) || lv_is_locked(lv) || lv_is_converting(lv) ||
	     lv_is_merging(lv)))
		lv_spawn_background_polling(cmd, lv);

	return 1;
}

static int detach_metadata_devices(struct lv_segment *seg, struct dm_list *list)
{
	uint32_t s;
	uint32_t num_meta_lvs;
	struct lv_list *lvl;

	num_meta_lvs = seg_is_raid(seg) ? seg->area_count : !!seg->log_lv;

	if (!num_meta_lvs)
		return_0;

	if (!(lvl = dm_pool_alloc(seg->lv->vg->vgmem, sizeof(*lvl) * num_meta_lvs)))
		return_0;

	if (seg_is_raid_with_meta(seg)) {
		for (s = 0; s < seg->area_count; s++) {
			if (!seg_metalv(seg, s))
				return_0; /* Trap this future possibility */

			lvl[s].lv = seg_metalv(seg, s);
			lv_set_visible(lvl[s].lv);

			dm_list_add(list, &lvl[s].list);
		}
		return 1;
	}

	lvl[0].lv = detach_mirror_log(seg);
	dm_list_add(list, &lvl[0].list);

	return 1;
}

static int attach_metadata_devices(struct lv_segment *seg, struct dm_list *list)
{
	struct lv_list *lvl;

	if (seg_is_raid(seg)) {
		dm_list_iterate_items(lvl, list)
			lv_set_hidden(lvl->lv);
		return 1;
	}

	dm_list_iterate_items(lvl, list)
		break;  /* get first item */

	if (!attach_mirror_log(seg, lvl->lv))
		return_0;

	return 1;
}

/*
 * lvchange_refresh
 * @cmd
 * @lv
 *
 * Suspend and resume a logical volume.
 */
static int _lvchange_refresh(struct cmd_context *cmd, struct logical_volume *lv)
{
	log_verbose("Refreshing logical volume %s (if active).", display_lvname(lv));

	return lv_refresh(cmd, lv);
}

static int _reactivate_lv(struct logical_volume *lv,
			  int active, int exclusive)
{
	struct cmd_context *cmd = lv->vg->cmd;

	if (!active)
		return 1;

	if (exclusive)
		return activate_lv_excl_local(cmd, lv);

	return activate_lv(cmd, lv);
}

/*
 * lvchange_resync
 * @cmd
 * @lv
 *
 * Force a mirror or RAID array to undergo a complete initializing resync.
 */
static int _lvchange_resync(struct cmd_context *cmd, struct logical_volume *lv)
{
	int active = 0;
	int exclusive = 0;
	int monitored;
	struct lv_segment *seg = first_seg(lv);
	struct dm_list device_list;
	struct lv_list *lvl;

	dm_list_init(&device_list);

	if (seg_is_any_raid0(seg) ||
	    (!seg_is_mirror(seg) && !seg_is_raid(seg))) {
		log_error("Unable to resync %s.  It is not RAID4/5/6/10 or mirrored.",
			  display_lvname(lv));
		return 0;
	}

	if (lv_is_pvmove(lv)) {
		log_error("Unable to resync pvmove volume %s.", display_lvname(lv));
		return 0;
	}

	if (lv_is_locked(lv)) {
		log_error("Unable to resync locked volume %s.", display_lvname(lv));
		return 0;
	}

	if (lv_is_active_locally(lv)) {
		if (!lv_check_not_in_use(lv, 1)) {
			log_error("Can't resync open logical volume %s.",
				  display_lvname(lv));
			return 0;
		}

		if (!arg_is_set(cmd, yes_ARG) &&
		    yes_no_prompt("Do you really want to deactivate "
				  "logical volume %s to resync it? [y/n]: ",
				  display_lvname(lv)) == 'n') {
			log_error("Logical volume %s not resynced.",
				  display_lvname(lv));
			return 0;
		}

		active = 1;
		if (lv_is_active_exclusive_locally(lv))
			exclusive = 1;
	}

	if (seg_is_raid(seg) && active && !exclusive) {
		log_error("RAID logical volume %s cannot be active remotely.",
			  display_lvname(lv));
		return 0;
	}

	/* Activate exclusively to ensure no nodes still have LV active */
	monitored = dmeventd_monitor_mode();
	if (monitored != DMEVENTD_MONITOR_IGNORE)
		init_dmeventd_monitor(0);

	if (!deactivate_lv(cmd, lv)) {
		log_error("Unable to deactivate %s for resync.", display_lvname(lv));
		return 0;
	}

	if (vg_is_clustered(lv->vg) && lv_is_active(lv)) {
		log_error("Can't get exclusive access to clustered volume %s.",
			  display_lvname(lv));
		return 0;
	}

	if (monitored != DMEVENTD_MONITOR_IGNORE)
		init_dmeventd_monitor(monitored);
	init_mirror_in_sync(0);

	log_very_verbose("Starting resync of %s%s%s%s %s.",
			 (active) ? "active " : "",
			 vg_is_clustered(lv->vg) ? "clustered " : "",
			 (seg->log_lv) ? "disk-logged " :
			 seg_is_raid(seg) ? "" : "core-logged ",
			 lvseg_name(seg), display_lvname(lv));

	/*
	 * If this mirror has a core log (i.e. !seg->log_lv),
	 * then simply deactivating/activating will cause
	 * it to reset the sync status.  We only need to
	 * worry about persistent logs.
	 */
	if (!seg_is_raid(seg) && !seg->log_lv) {
		if (lv_is_not_synced(lv)) {
			lv->status &= ~LV_NOTSYNCED;
			log_very_verbose("Updating logical volume %s on disk(s).",
					 display_lvname(lv));
			if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
				log_error("Failed to update metadata on disk.");
				return 0;
			}
		}

		if (!_reactivate_lv(lv, active, exclusive)) {
			log_error("Failed to reactivate %s to resynchronize mirror.",
				  display_lvname(lv));
			return 0;
		}

		return 1;
	}

	/*
	 * Now we handle mirrors with log devices
	 */
	lv->status &= ~LV_NOTSYNCED;

	/* Separate mirror log or metadata devices so we can clear them */
	if (!detach_metadata_devices(seg, &device_list)) {
		log_error("Failed to clear %s %s for %s.",
			  lvseg_name(seg), seg_is_raid(seg) ?
			  "metadata area" : "mirror log", display_lvname(lv));
		return 0;
	}

	if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
		log_error("Failed to update intermediate VG metadata on disk.");
		if (!_reactivate_lv(lv, active, exclusive))
			stack;
		return 0;
	}

	/* No backup for intermediate metadata, so just unlock memory */
	memlock_unlock(lv->vg->cmd);

	dm_list_iterate_items(lvl, &device_list) {
		if (!activate_lv_excl_local(cmd, lvl->lv)) {
			log_error("Unable to activate %s for %s clearing.",
				  display_lvname(lvl->lv), (seg_is_raid(seg)) ?
				  "metadata area" : "mirror log");
			return 0;
		}

		if (!wipe_lv(lvl->lv, (struct wipe_params)
			     { .do_zero = 1, .zero_sectors = lvl->lv->size })) {
			log_error("Unable to reset sync status for %s.",
				  display_lvname(lv));
			if (!deactivate_lv(cmd, lvl->lv))
				log_error("Failed to deactivate log LV after "
					  "wiping failed");
			return 0;
		}

		if (!deactivate_lv(cmd, lvl->lv)) {
			log_error("Unable to deactivate %s LV %s "
				  "after wiping for resync.",
				  (seg_is_raid(seg)) ? "metadata" : "log",
				  display_lvname(lvl->lv));
			return 0;
		}
	}

	/* Wait until devices are away */
	if (!sync_local_dev_names(lv->vg->cmd)) {
		log_error("Failed to sync local devices after updating %s.",
			  display_lvname(lv));
		return 0;
	}

	/* Put metadata sub-LVs back in place */
	if (!attach_metadata_devices(seg, &device_list)) {
		log_error("Failed to reattach %s device after clearing.",
			  (seg_is_raid(seg)) ? "metadata" : "log");
		return 0;
	}

	if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
		log_error("Failed to update metadata on disk for %s.",
			  display_lvname(lv));
		return 0;
	}

	if (!_reactivate_lv(lv, active, exclusive)) {
		backup(lv->vg);
		log_error("Failed to reactivate %s after resync.",
			  display_lvname(lv));
		return 0;
	}

	backup(lv->vg);

	return 1;
}

static int _lvchange_alloc(struct cmd_context *cmd, struct logical_volume *lv)
{
	int want_contiguous = arg_int_value(cmd, contiguous_ARG, 0);
	alloc_policy_t alloc = (alloc_policy_t)
		arg_uint_value(cmd, alloc_ARG, (want_contiguous)
			       ? ALLOC_CONTIGUOUS : ALLOC_INHERIT);

	if (alloc == lv->alloc) {
		log_error("Allocation policy of logical volume %s is already %s.",
			  display_lvname(lv), get_alloc_string(alloc));
		return 0;
	}

	lv->alloc = alloc;

	/* FIXME If contiguous, check existing extents already are */

	log_verbose("Setting contiguous allocation policy for %s to %s.",
		    display_lvname(lv), get_alloc_string(alloc));

	log_very_verbose("Updating logical volume %s on disk(s).", display_lvname(lv));

	/* No need to suspend LV for this change */
	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	return 1;
}

static int _lvchange_errorwhenfull(struct cmd_context *cmd,
				   struct logical_volume *lv)
{
	unsigned ewf = arg_int_value(cmd, errorwhenfull_ARG, 0);

	if (ewf == lv_is_error_when_full(lv)) {
		log_error("Error when full is already %sset for %s.",
			  (ewf) ? "" : "un", display_lvname(lv));
		return 0;
	}

	if (ewf)
		lv->status |= LV_ERROR_WHEN_FULL;
	else
		lv->status &= ~LV_ERROR_WHEN_FULL;

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

static int _lvchange_readahead(struct cmd_context *cmd,
			       struct logical_volume *lv)
{
	unsigned read_ahead = 0;
	unsigned pagesize = (unsigned) lvm_getpagesize() >> SECTOR_SHIFT;

	read_ahead = arg_uint_value(cmd, readahead_ARG, 0);

	if (read_ahead != DM_READ_AHEAD_AUTO &&
	    (lv->vg->fid->fmt->features & FMT_RESTRICTED_READAHEAD) &&
	    (read_ahead < 2 || read_ahead > 120)) {
		log_error("Metadata only supports readahead values between 2 and 120.");
		return 0;
	}

	if (read_ahead != DM_READ_AHEAD_AUTO &&
	    read_ahead != DM_READ_AHEAD_NONE && read_ahead % pagesize) {
		if (read_ahead < pagesize)
			read_ahead = pagesize;
		else
			read_ahead = (read_ahead / pagesize) * pagesize;
		log_warn("WARNING: Overriding readahead to %u sectors, a multiple "
			    "of %uK page size.", read_ahead, pagesize >> 1);
	}

	if (lv->read_ahead == read_ahead) {
		if (read_ahead == DM_READ_AHEAD_AUTO)
			log_error("Read ahead is already auto for %s.",
				  display_lvname(lv));
		else
			log_error("Read ahead is already %u for %s.",
				  read_ahead, display_lvname(lv));
		return 0;
	}

	lv->read_ahead = read_ahead;

	log_verbose("Setting read ahead to %u for %s.",
		    read_ahead, display_lvname(lv));

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

static int _lvchange_persistent(struct cmd_context *cmd,
				struct logical_volume *lv)
{
	enum activation_change activate = CHANGE_AN;

	/* The LV lock in lvmlockd should remain as it is. */
	cmd->lockd_lv_disable = 1;

	if (!get_and_validate_major_minor(cmd, lv->vg->fid->fmt,
					  &lv->major, &lv->minor))
		return_0;

	if (lv->minor == -1) {
		if (!(lv->status & FIXED_MINOR)) {
			log_error("Minor number is already not persistent for %s.",
				  display_lvname(lv));
			return 0;
		}
		lv->status &= ~FIXED_MINOR;
		log_verbose("Disabling persistent device number for %s.",
			    display_lvname(lv));
	} else {
		if (lv_is_active(lv)) {
			if (!arg_is_set(cmd, force_ARG) &&
			    !arg_is_set(cmd, yes_ARG) &&
			    yes_no_prompt("Logical volume %s will be "
					  "deactivated temporarily. "
					  "Continue? [y/n]: ",
					  display_lvname(lv)) == 'n') {
				log_error("%s device number not changed.",
					  display_lvname(lv));
				return 0;
			}

			activate = CHANGE_AEY;
			if (vg_is_clustered(lv->vg) &&
			    locking_is_clustered() &&
			    locking_supports_remote_queries() &&
			    !lv_is_active_exclusive_locally(lv)) {
				/* Reliable reactivate only locally */
				log_print_unless_silent("Remotely active LV %s needs "
							"individual reactivation.",
							display_lvname(lv));
				activate = CHANGE_ALY;
			}
		}

		/* Ensuring LV is not active */
		if (!deactivate_lv(cmd, lv)) {
			log_error("Cannot deactivate %s.", display_lvname(lv));
			return 0;
		}
		lv->status |= FIXED_MINOR;
		log_verbose("Setting persistent device number to (%d, %d) for %s.",
			    lv->major, lv->minor, display_lvname(lv));
	}

	log_very_verbose("Updating logical volume %s on disk(s).",
			 display_lvname(lv));

	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	if (activate != CHANGE_AN) {
		log_verbose("Re-activating logical volume %s.", display_lvname(lv));
		if (!lv_active_change(cmd, lv, activate, 0)) {
			log_error("%s: reactivation failed.", display_lvname(lv));
			backup(lv->vg);
			return 0;
		}
	}

	backup(lv->vg);

	return 1;
}

static int _lvchange_cache(struct cmd_context *cmd, struct logical_volume *lv)
{
	cache_mode_t mode;
	const char *name;
	struct dm_config_tree *settings = NULL;
	struct lv_segment *pool_seg = first_seg(lv);
	int r = 0, is_clean;

	if (lv_is_cache(lv))
		pool_seg = first_seg(pool_seg->pool_lv);
        else if (!lv_is_cache_pool(lv)) {
		log_error("LV %s is not a cache LV.", display_lvname(lv));
		(void) arg_from_list_is_set(cmd, "is supported only with cache or cache pool LVs",
					    cachemode_ARG,
					    cachepolicy_ARG,
					    cachesettings_ARG,
					    -1);
		goto out;
	}

	if (!get_cache_params(cmd, &mode, &name, &settings))
		goto_out;

	if ((mode != CACHE_MODE_UNDEFINED) &&
	    (mode != pool_seg->cache_mode) &&
	    lv_is_cache(lv)) {
		if (!lv_cache_wait_for_clean(lv, &is_clean))
			return_0;
		if (!is_clean) {
			log_error("Cache %s is not clean, refusing to switch cache mode.",
				  display_lvname(lv));
			return 0;
		}
	}

	if (mode && !cache_set_cache_mode(first_seg(lv), mode))
		goto_out;

	if ((name || settings) &&
	    !cache_set_policy(first_seg(lv), name, settings))
		goto_out;

	if (!lv_update_and_reload(lv))
		goto_out;

	r = 1;
out:
	if (settings)
		dm_config_destroy(settings);

	return r;
}

static int _lvchange_tag(struct cmd_context *cmd, struct logical_volume *lv, int arg)
{
	if (!change_tag(cmd, NULL, lv, NULL, arg))
		return_0;

	log_very_verbose("Updating logical volume %s on disk(s).", display_lvname(lv));

	/* No need to suspend LV for this change */
	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	return 1;
}

static int _lvchange_rebuild(struct logical_volume *lv)
{
	int pv_count, i = 0;
	char **rebuild_pvs;
	const char *tmp_str;
	struct dm_list *rebuild_pvh = NULL;
	struct arg_value_group_list *group;
	struct volume_group *vg = lv->vg;
	struct cmd_context *cmd = vg->cmd;
	struct lv_segment *raid_seg = first_seg(lv);

	if (!seg_is_raid(raid_seg) || seg_is_any_raid0(raid_seg)) {
		log_error("--rebuild can only be used with 'raid4/5/6/10' segment types.");
		return 0;
	}

	if (!(pv_count = arg_count(cmd, rebuild_ARG))) {
		log_error("No --rebuild found!");
		return 0;
	}

	if (!arg_is_set(cmd, yes_ARG) &&
	    yes_no_prompt("Do you really want to rebuild %u PVs "
			  "of logical volume %s [y/n]: ",
			  pv_count, display_lvname(lv)) == 'n') {
		log_error("Logical volume %s not rebuild.",
			  display_lvname(lv));
		return 0;
	}

	/* rebuild can be specified more than once */
	if (!(rebuild_pvs = dm_pool_alloc(vg->vgmem, sizeof(char *) * pv_count)))
		return_0;

	dm_list_iterate_items(group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(group->arg_values, rebuild_ARG))
			continue;

		if (!(tmp_str = grouped_arg_str_value(group->arg_values,
						      rebuild_ARG, NULL)))
			return_0;

		if (!(rebuild_pvs[i++] = dm_pool_strdup(cmd->mem, tmp_str)))
                               return_0;
	}

	if (!(rebuild_pvh = create_pv_list(cmd->mem, vg,
					   pv_count, rebuild_pvs, 0)))
		return_ECMD_FAILED;

	/* Rebuild PVs listed on @rebuild_pvh */
	return lv_raid_rebuild(lv, rebuild_pvh);
}

static int _lvchange_writemostly(struct logical_volume *lv)
{
	int s, pv_count, i = 0;
	char **pv_names;
	const char *tmp_str;
	size_t tmp_str_len;
	struct pv_list *pvl;
	struct arg_value_group_list *group;
	struct cmd_context *cmd = lv->vg->cmd;
	struct lv_segment *raid_seg = first_seg(lv);

	if (!seg_is_raid1(raid_seg)) {
		log_error("--write%s can only be used with 'raid1' segment type.",
			  arg_is_set(cmd, writemostly_ARG) ? "mostly" : "behind");
		return 0;
	}

	if (arg_is_set(cmd, writebehind_ARG))
		raid_seg->writebehind = arg_uint_value(cmd, writebehind_ARG, 0);

	if ((pv_count = arg_count(cmd, writemostly_ARG))) {
		/* writemostly can be specified more than once */
		pv_names = dm_pool_alloc(lv->vg->vgmem, sizeof(char *) * pv_count);
		if (!pv_names)
			return_0;

		dm_list_iterate_items(group, &cmd->arg_value_groups) {
			if (!grouped_arg_is_set(group->arg_values,
						writemostly_ARG))
				continue;

			if (!(tmp_str = grouped_arg_str_value(group->arg_values,
							      writemostly_ARG,
							      NULL)))
				return_0;

			/*
			 * Writemostly PV specifications can be:
			 *   <PV>   - Turn on writemostly
			 *   <PV>:t - Toggle writemostly
			 *   <PV>:n - Turn off writemostly
			 *   <PV>:y - Turn on writemostly
			 *
			 * We allocate strlen + 3 to add our own ':{t|n|y}' if
			 * not present plus the trailing '\0'.
			 */
			tmp_str_len = strlen(tmp_str);
			if (!(pv_names[i] = dm_pool_zalloc(lv->vg->vgmem, tmp_str_len + 3)))
				return_0;

			if ((tmp_str_len < 3) ||
			    (tmp_str[tmp_str_len - 2] != ':'))
				/* Default to 'y' if no mode specified */
				sprintf(pv_names[i], "%s:y", tmp_str);
			else
				sprintf(pv_names[i], "%s", tmp_str);
			i++;
		}

		for (i = 0; i < pv_count; i++)
			pv_names[i][strlen(pv_names[i]) - 2] = '\0';

		for (i = 0; i < pv_count; i++) {
			if (!(pvl = find_pv_in_vg(lv->vg, pv_names[i]))) {
				log_error("%s not found in volume group, %s",
					  pv_names[i], lv->vg->name);
				return 0;
			}

			for (s = 0; s < (int) raid_seg->area_count; s++) {
				/*
				 * We don't bother checking the metadata area,
				 * since writemostly only affects the data areas.
				 */
				if (seg_type(raid_seg, s) == AREA_UNASSIGNED)
					continue;

				if (lv_is_on_pv(seg_lv(raid_seg, s), pvl->pv)) {
					if (pv_names[i][strlen(pv_names[i]) + 1] == 'y')
						seg_lv(raid_seg, s)->status |=
							LV_WRITEMOSTLY;
					else if (pv_names[i][strlen(pv_names[i]) + 1] == 'n')
						seg_lv(raid_seg, s)->status &=
							~LV_WRITEMOSTLY;
					else if (pv_names[i][strlen(pv_names[i]) + 1] == 't')
						seg_lv(raid_seg, s)->status ^=
							LV_WRITEMOSTLY;
					else
						return_0;
				}
			}
		}
	}

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

static int _lvchange_recovery_rate(struct logical_volume *lv)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct lv_segment *raid_seg = first_seg(lv);

	if (!seg_is_raid(raid_seg)) {
		log_error("Unable to change the recovery rate of non-RAID "
			  "logical volume %s.", display_lvname(lv));
		return 0;
	}

	if (arg_is_set(cmd, minrecoveryrate_ARG))
		raid_seg->min_recovery_rate =
			arg_uint_value(cmd, minrecoveryrate_ARG, 0) / 2;
	if (arg_is_set(cmd, maxrecoveryrate_ARG))
		raid_seg->max_recovery_rate =
			arg_uint_value(cmd, maxrecoveryrate_ARG, 0) / 2;

	if (raid_seg->max_recovery_rate &&
	    (raid_seg->max_recovery_rate < raid_seg->min_recovery_rate)) {
		log_error("Minimum recovery rate cannot be higher than maximum.");
		return 0;
	}

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

static int _lvchange_profile(struct logical_volume *lv)
{
	const char *old_profile_name, *new_profile_name;
	struct profile *new_profile;

	old_profile_name = lv->profile ? lv->profile->name : "(inherited)";

	if (arg_is_set(lv->vg->cmd, detachprofile_ARG)) {
		new_profile_name = "(inherited)";
		lv->profile = NULL;
	} else {
		if (arg_is_set(lv->vg->cmd, metadataprofile_ARG))
			new_profile_name = arg_str_value(lv->vg->cmd, metadataprofile_ARG, NULL);
		else
			new_profile_name = arg_str_value(lv->vg->cmd, profile_ARG, NULL);
		if (!(new_profile = add_profile(lv->vg->cmd, new_profile_name, CONFIG_PROFILE_METADATA)))
			return_0;
		lv->profile = new_profile;
	}

	log_verbose("Changing configuration profile for LV %s: %s -> %s.",
		    display_lvname(lv), old_profile_name, new_profile_name);

	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	return 1;
}

static int _lvchange_activation_skip(struct logical_volume *lv)
{
	int skip = arg_int_value(lv->vg->cmd, setactivationskip_ARG, 0);

	lv_set_activation_skip(lv, 1, skip);

	log_verbose("Changing activation skip flag to %s for LV %s.",
		    display_lvname(lv), skip ? "enabled" : "disabled");

	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	return 1;
}


static int _lvchange_single(struct cmd_context *cmd, struct logical_volume *lv,
			    struct processing_handle *handle __attribute__((unused)))
{
	int doit = 0, docmds = 0;
	struct logical_volume *origin;
	char snaps_msg[128];

	if (sigint_caught())
		return_ECMD_FAILED;

	if (!(lv->vg->status & LVM_WRITE) &&
	    arg_from_list_is_set(cmd, NULL,
				 alloc_ARG,
				 contiguous_ARG,
				 discards_ARG,
				 metadataprofile_ARG,
				 permission_ARG,
				 persistent_ARG,
				 profile_ARG,
				 readahead_ARG,
				 zero_ARG,
				 -1)) {
		log_error("Only -a permitted with read-only volume group %s.",
			  lv->vg->name);
		return ECMD_FAILED;
	}

	if (lv_is_origin(lv) && !lv_is_thin_volume(lv) &&
	    arg_from_list_is_set(cmd, NULL,
				 alloc_ARG,
				 contiguous_ARG,
				 metadataprofile_ARG,
				 permission_ARG,
				 persistent_ARG,
				 profile_ARG,
				 readahead_ARG,
				 -1)) {
		log_error("Can't change logical volume %s under snapshot.",
			  display_lvname(lv));
		return ECMD_FAILED;
	}

	if (lv_is_pvmove(lv)) {
		log_error("Unable to change pvmove LV %s.", display_lvname(lv));
		if (arg_is_set(cmd, activate_ARG))
			log_error("Use 'pvmove --abort' to abandon a pvmove");
		return ECMD_FAILED;
	}

	if (lv_is_mirror_log(lv)) {
		log_error("Unable to change mirror log LV %s directly.",
			  display_lvname(lv));
		return ECMD_FAILED;
	}

	if (lv_is_mirror_image(lv)) {
		log_error("Unable to change mirror image LV %s directly.",
			  display_lvname(lv));
		return ECMD_FAILED;
	}

	/* If LV is sparse, activate origin instead */
	if (arg_is_set(cmd, activate_ARG) && lv_is_cow(lv) &&
	    lv_is_virtual_origin(origin = origin_from_cow(lv)))
		lv = origin;

	/* Use cache origin LV for 'raid' actions */
	if (lv_is_cache(lv) &&
	    arg_from_list_is_set(cmd, NULL,
				 /* FIXME: we want to support more ops here */
				 //resync_ARG,
				 syncaction_ARG,
				 -1)) {
		lv = seg_lv(first_seg(lv), 0);
		log_debug("Using cache origin volume %s for lvchange instead.",
			  display_lvname(lv));
	}

	if ((lv_is_thin_pool_data(lv) || lv_is_thin_pool_metadata(lv) ||
	     lv_is_cache_pool_data(lv) || lv_is_cache_pool_metadata(lv)) &&
	    !arg_is_set(cmd, activate_ARG) &&
	    !arg_is_set(cmd, permission_ARG) &&
	    !arg_is_set(cmd, setactivationskip_ARG))
	    /* Rest can be changed for stacked thin pool meta/data volumes */
	    ;
	else if (lv_is_cache_origin(lv) && lv_is_raid(lv)) {
		if (vg_is_clustered(lv->vg)) {
			log_error("Unable to change internal LV %s directly in a cluster.",
				  display_lvname(lv));
			return ECMD_FAILED;
		}
		/*
		 * FIXME:  For now, we don't want to allow all kinds of
		 * operations on this cache origin sub-LV.  We are going
		 * to restrict it to non-clustered, RAID.  This way, we
		 * can change the syncaction as needed (e.g. initiate
		 * scrubbing).
		 *
		 * Later pass all 'cache' actions on cache origin.
		 */
	} else if (!lv_is_visible(lv) && !lv_is_virtual_origin(lv)) {
		log_error("Unable to change internal LV %s directly.",
			  display_lvname(lv));
		return ECMD_FAILED;
	}

	if (lv_is_cow(lv) && arg_is_set(cmd, activate_ARG)) {
		origin = origin_from_cow(lv);
		if (origin->origin_count < 2)
			snaps_msg[0] = '\0';
		else if (dm_snprintf(snaps_msg, sizeof(snaps_msg),
				     " and %u other snapshot(s)",
				     origin->origin_count - 1) < 0) {
			log_error("Failed to prepare message.");
			return ECMD_FAILED;
		}

		if (!arg_is_set(cmd, yes_ARG) &&
		    (yes_no_prompt("Change of snapshot %s will also change its "
				   "origin %s%s. Proceed? [y/n]: ",
				   display_lvname(lv), display_lvname(origin),
				   snaps_msg) == 'n')) {
			log_error("Logical volume %s not changed.", display_lvname(lv));
			return ECMD_FAILED;
		}
	}

	if (arg_is_set(cmd, errorwhenfull_ARG) && !lv_is_thin_pool(lv)) {
		log_error("Option --errorwhenfull is only supported with thin pools.");
		return ECMD_FAILED;
	}

	if (arg_is_set(cmd, persistent_ARG) && lv_is_pool(lv)) {
		log_error("Persistent major and minor numbers are not supported with pools.");
		return ECMD_FAILED;
	}

	if (!arg_is_set(cmd, activate_ARG) && !arg_is_set(cmd, refresh_ARG)) {
		/*
		 * If a persistent lv lock already exists from activation
		 * (with the needed mode or higher), this will be a no-op.
		 * Otherwise, the lv lock will be taken as non-persistent
		 * and released when this command exits.
		 *
		 * FIXME: use "sh" if the options imply that the lvchange
		 * operation does not modify the LV.
		 */
		if (!lockd_lv(cmd, lv, "ex", 0)) {
			stack;
			return ECMD_FAILED;
		}
	}

	/*
	 * FIXME: DEFAULT_BACKGROUND_POLLING should be "unspecified".
	 * If --poll is explicitly provided use it; otherwise polling
	 * should only be started if the LV is not already active. So:
	 * 1) change the activation code to say if the LV was actually activated
	 * 2) make polling of an LV tightly coupled with LV activation
	 *
	 * Do not initiate any polling if --sysinit option is used.
	 */
	init_background_polling(arg_is_set(cmd, sysinit_ARG) ? 0 :
						arg_int_value(cmd, poll_ARG,
						DEFAULT_BACKGROUND_POLLING));

	/* access permission change */
	if (arg_is_set(cmd, permission_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_permission(cmd, lv);
		docmds++;
	}

	/* allocation policy change */
	if (arg_is_set(cmd, contiguous_ARG) || arg_is_set(cmd, alloc_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_alloc(cmd, lv);
		docmds++;
	}

	/* error when full change */
	if (arg_is_set(cmd, errorwhenfull_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_errorwhenfull(cmd, lv);
		docmds++;
	}

	/* read ahead sector change */
	if (arg_is_set(cmd, readahead_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_readahead(cmd, lv);
		docmds++;
	}

	/* persistent device number change */
	if (arg_is_set(cmd, persistent_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_persistent(cmd, lv);
		docmds++;
	}

	if (arg_is_set(cmd, discards_ARG) ||
	    arg_is_set(cmd, zero_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_pool_update(cmd, lv);
		docmds++;
	}

	/* add tag */
	if (arg_is_set(cmd, addtag_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_tag(cmd, lv, addtag_ARG);
		docmds++;
	}

	/* del tag */
	if (arg_is_set(cmd, deltag_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_tag(cmd, lv, deltag_ARG);
		docmds++;
	}

	/* rebuild selected PVs */
	if (arg_is_set(cmd, rebuild_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_rebuild(lv);
		docmds++;
	}

	/* change writemostly/writebehind */
	if (arg_is_set(cmd, writemostly_ARG) || arg_is_set(cmd, writebehind_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_writemostly(lv);
		docmds++;
	}

	/* change [min|max]_recovery_rate */
	if (arg_is_set(cmd, minrecoveryrate_ARG) ||
	    arg_is_set(cmd, maxrecoveryrate_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_recovery_rate(lv);
		docmds++;
	}

	/* change configuration profile */
	if (arg_is_set(cmd, profile_ARG) || arg_is_set(cmd, metadataprofile_ARG) ||
	    arg_is_set(cmd, detachprofile_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_profile(lv);
		docmds++;
	}

	if (arg_is_set(cmd, setactivationskip_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_activation_skip(lv);
		docmds++;
	}

	if (arg_is_set(cmd, cachemode_ARG) ||
	    arg_is_set(cmd, cachepolicy_ARG) || arg_is_set(cmd, cachesettings_ARG)) {
		if (!archive(lv->vg))
			return_ECMD_FAILED;
		doit += _lvchange_cache(cmd, lv);
		docmds++;
	}

	if (doit)
		log_print_unless_silent("Logical volume %s changed.", display_lvname(lv));

	if (arg_is_set(cmd, resync_ARG) &&
	    !_lvchange_resync(cmd, lv))
		return_ECMD_FAILED;

	if (arg_is_set(cmd, syncaction_ARG)) {
		struct lv_segment *seg = first_seg(lv);

		if (seg_is_any_raid0(seg)) {
			log_error("Unable to sync raid0 LV %s.", display_lvname(lv));
			return_ECMD_FAILED;
		}
		
		if (!lv_raid_message(lv, arg_str_value(cmd, syncaction_ARG, NULL)))
			return_ECMD_FAILED;
	}

	/* activation change */
	if (arg_is_set(cmd, activate_ARG)) {
		if (!_lvchange_activate(cmd, lv))
			return_ECMD_FAILED;
	} else if (arg_is_set(cmd, refresh_ARG)) {
		if (!_lvchange_refresh(cmd, lv))
			return_ECMD_FAILED;
	} else {
		if (arg_is_set(cmd, monitor_ARG) &&
		    !_lvchange_monitoring(cmd, lv))
			return_ECMD_FAILED;

		if (arg_is_set(cmd, poll_ARG) &&
		    !_lvchange_background_polling(cmd, lv))
			return_ECMD_FAILED;
	}

	if (doit != docmds)
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

int lvchange(struct cmd_context *cmd, int argc, char **argv)
{
	/*
	 * Options that update metadata should be listed in one of
	 * the two lists below (i.e. options other than -a, --refresh,
	 * --monitor or --poll).
	 */
	int update_partial_safe = /* options safe to update if partial */
		arg_from_list_is_set(cmd, NULL,
				     addtag_ARG,
				     contiguous_ARG,
				     deltag_ARG,
				     detachprofile_ARG,
				     metadataprofile_ARG,
				     permission_ARG,
				     persistent_ARG,
				     profile_ARG,
				     readahead_ARG,
				     setactivationskip_ARG,
				     -1);
	int update_partial_unsafe =
		arg_from_list_is_set(cmd, NULL,
				     alloc_ARG,
				     cachemode_ARG,
				     cachepolicy_ARG,
				     cachesettings_ARG,
				     discards_ARG,
				     errorwhenfull_ARG,
				     maxrecoveryrate_ARG,
				     minrecoveryrate_ARG,
				     rebuild_ARG,
				     resync_ARG,
				     syncaction_ARG,
				     writebehind_ARG,
				     writemostly_ARG,
				     zero_ARG,
				     -1);
	int update = update_partial_safe || update_partial_unsafe;

	if (!update &&
	    !arg_is_set(cmd, activate_ARG) && !arg_is_set(cmd, refresh_ARG) &&
	    !arg_is_set(cmd, monitor_ARG) && !arg_is_set(cmd, poll_ARG)) {
		log_error("Need 1 or more of -a, -C, -M, -p, -r, -Z, "
			  "--resync, --refresh, --alloc, --addtag, --deltag, "
			  "--monitor, --poll or --discards");
		return EINVALID_CMD_LINE;
	}

	if ((arg_is_set(cmd, profile_ARG) || arg_is_set(cmd, metadataprofile_ARG)) &&
	     arg_is_set(cmd, detachprofile_ARG)) {
		log_error("Only one of --metadataprofile and --detachprofile permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, activate_ARG) && arg_is_set(cmd, refresh_ARG)) {
		log_error("Only one of -a and --refresh permitted.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_is_set(cmd, ignorelockingfailure_ARG) ||
	     arg_is_set(cmd, sysinit_ARG)) && update) {
		log_error("Only -a permitted with --ignorelockingfailure and --sysinit");
		return EINVALID_CMD_LINE;
	}

	if (!update || !update_partial_unsafe)
		cmd->handles_missing_pvs = 1;

	if (!argc && !arg_is_set(cmd, select_ARG)) {
		log_error("Please give logical volume path(s) or use --select for selection.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_is_set(cmd, minor_ARG) || arg_is_set(cmd, major_ARG)) &&
	    !arg_is_set(cmd, persistent_ARG)) {
		log_error("--major and --minor require -My.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, minor_ARG) && argc != 1) {
		log_error("Only give one logical volume when specifying minor.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, contiguous_ARG) && arg_is_set(cmd, alloc_ARG)) {
		log_error("Only one of --alloc and --contiguous permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, poll_ARG) && arg_is_set(cmd, sysinit_ARG)) {
		log_error("Only one of --poll and --sysinit permitted.");
		return EINVALID_CMD_LINE;
	}

	/*
	 * If --sysinit -aay is used and at the same time lvmetad is used,
	 * we want to rely on autoactivation to take place. Also, we
	 * need to take special care here as lvmetad service does
	 * not neet to be running at this moment yet - it could be
	 * just too early during system initialization time.
	 */
	if (arg_is_set(cmd, sysinit_ARG) && (arg_uint_value(cmd, activate_ARG, 0) == CHANGE_AAY)) {
		if (lvmetad_used()) {
			log_warn("WARNING: lvmetad is active, skipping direct activation during sysinit.");
			return ECMD_PROCESSED;
		}
	}

	/*
	 * Include foreign VGs that contain active LVs.
	 * That shouldn't happen in general, but if it does by some
	 * mistake, then we want to allow those LVs to be deactivated.
	 */
	if (arg_is_set(cmd, activate_ARG))
		cmd->include_active_foreign_vgs = 1;

	/*
	 * The default vg lock mode for lvchange is ex, but these options
	 * are cases where lvchange does not modify the vg, so they can use
	 * the sh lock mode.
	 */
	if (arg_is_set(cmd, activate_ARG) || arg_is_set(cmd, refresh_ARG)) {
		cmd->lockd_vg_default_sh = 1;
		/* Allow deactivating if locks fail. */
		if (is_change_activating((activation_change_t)arg_uint_value(cmd, activate_ARG, CHANGE_AY)))
			cmd->lockd_vg_enforce_sh = 1;
	}

	return process_each_lv(cmd, argc, argv, NULL, NULL,
			       update ? READ_FOR_UPDATE : 0, NULL,
			       NULL, &_lvchange_single);
}
