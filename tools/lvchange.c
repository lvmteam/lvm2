/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
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

#include "tools.h"

static int lvchange_permission(struct cmd_context *cmd,
			       struct logical_volume *lv)
{
	uint32_t lv_access;
	struct lvinfo info;
	int r = 0;

	lv_access = arg_uint_value(cmd, permission_ARG, 0);

	if ((lv_access & LVM_WRITE) && (lv->status & LVM_WRITE)) {
		log_error("Logical volume \"%s\" is already writable",
			  lv->name);
		return 0;
	}

	if (!(lv_access & LVM_WRITE) && !(lv->status & LVM_WRITE)) {
		log_error("Logical volume \"%s\" is already read only",
			  lv->name);
		return 0;
	}

	if ((lv->status & MIRRORED) && (vg_is_clustered(lv->vg)) &&
	    lv_info(cmd, lv, 0, &info, 0, 0) && info.exists) {
		log_error("Cannot change permissions of mirror \"%s\" "
			  "while active.", lv->name);
		return 0;
	}

	/* Not allowed to change permissions on RAID sub-LVs directly */
	if ((lv->status & RAID_META) || (lv->status & RAID_IMAGE)) {
		log_error("Cannot change permissions of RAID %s \"%s\"",
			  (lv->status & RAID_IMAGE) ? "image" :
			  "metadata area", lv->name);
		return 0;
	}

	if (!(lv_access & LVM_WRITE) && lv_is_thin_pool(lv)) {
		log_error("Change permissions of thin pool \"%s\" not "
			  "yes supported.", lv->name);
		return 0;
	}

	if (lv_access & LVM_WRITE) {
		lv->status |= LVM_WRITE;
		log_verbose("Setting logical volume \"%s\" read/write",
			    lv->name);
	} else {
		lv->status &= ~LVM_WRITE;
		log_verbose("Setting logical volume \"%s\" read-only",
			    lv->name);
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg))
		return_0;

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		goto out;
	}

	if (!vg_commit(lv->vg)) {
		if (!resume_lv(cmd, lv))
			stack;
		goto_out;
	}

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		goto out;
	}

	r = 1;
out:
	backup(lv->vg);
	return r;
}

static int lvchange_pool_update(struct cmd_context *cmd,
				struct logical_volume *lv)
{
	int r = 0;
	int update = 0;
	unsigned val;
	thin_discards_t discards;

	if (!lv_is_thin_pool(lv)) {
		log_error("Logical volume \"%s\" is not a thin pool.", lv->name);
		return 0;
	}

	if (arg_count(cmd, discards_ARG)) {
		discards = (thin_discards_t) arg_uint_value(cmd, discards_ARG, THIN_DISCARDS_IGNORE);
		if (discards != first_seg(lv)->discards) {
			if ((discards != THIN_DISCARDS_IGNORE) &&
				 (first_seg(lv)->chunk_size &
				  (first_seg(lv)->chunk_size - 1)))
				log_error("Cannot change discards state for "
					  "logical volume \"%s\" "
					  "with non power of 2 chunk size.", lv->name);
			else if (((discards == THIN_DISCARDS_IGNORE) ||
			     (first_seg(lv)->discards == THIN_DISCARDS_IGNORE)) &&
			    lv_is_active(lv))
				log_error("Cannot change discards state for active "
					  "logical volume \"%s\".", lv->name);
			else {
				first_seg(lv)->discards = discards;
				update++;
			}
		} else
			log_error("Logical volume \"%s\" already uses --discards %s.",
				  lv->name, get_pool_discards_name(discards));
	}

	if (arg_count(cmd, zero_ARG)) {
		val = arg_uint_value(cmd, zero_ARG, 1);
		if (val != first_seg(lv)->zero_new_blocks) {
			first_seg(lv)->zero_new_blocks = val;
			update++;
		} else
			log_error("Logical volume \"%s\" already %szero new blocks.",
				  lv->name, val ? "" : "does not ");
	}

	if (!update)
		return 0;

	log_very_verbose("Updating logical volume \"%s\" on disk(s).", lv->name);
	if (!vg_write(lv->vg))
		return_0;

	if (!suspend_lv_origin(cmd, lv)) {
		log_error("Failed to update active %s/%s (deactivation is needed).",
			  lv->vg->name, lv->name);
		vg_revert(lv->vg);
		goto out;
	}

	if (!vg_commit(lv->vg)) {
		if (!resume_lv_origin(cmd, lv))
			stack;
		goto_out;
	}

	if (!resume_lv_origin(cmd, lv)) {
		log_error("Problem reactivating %s.", lv->name);
		goto out;
	}

	r = 1;
out:
	backup(lv->vg);
	return r;
}

static int lvchange_monitoring(struct cmd_context *cmd,
			       struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, lv_is_thin_pool(lv) ? 1 : 0,
		     &info, 0, 0) || !info.exists) {
		log_error("Logical volume, %s, is not active", lv->name);
		return 0;
	}

	/* do not monitor pvmove lv's */
	if (lv->status & PVMOVE)
		return 1;

	if ((dmeventd_monitor_mode() != DMEVENTD_MONITOR_IGNORE) &&
	    !monitor_dev_for_events(cmd, lv, 0, dmeventd_monitor_mode()))
		return_0;

	return 1;
}

static int lvchange_background_polling(struct cmd_context *cmd,
				       struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, 0, &info, 0, 0) || !info.exists) {
		log_error("Logical volume, %s, is not active", lv->name);
		return 0;
	}

	if (background_polling())
		lv_spawn_background_polling(cmd, lv);

	return 1;
}

static int _lvchange_activate(struct cmd_context *cmd, struct logical_volume *lv)
{
	int activate;

	activate = arg_uint_value(cmd, activate_ARG, 0);

	if (lv_is_cow(lv) && !lv_is_virtual_origin(origin_from_cow(lv)))
		lv = origin_from_cow(lv);

	if (activate == CHANGE_AAY) {
		if (!lv_passes_auto_activation_filter(cmd, lv))
			return 1;
		activate = CHANGE_ALY;
	}

	if (activate == CHANGE_ALN) {
		log_verbose("Deactivating logical volume \"%s\" locally",
			    lv->name);
		if (!deactivate_lv_local(cmd, lv))
			return_0;
	} else if (activate == CHANGE_AN) {
		log_verbose("Deactivating logical volume \"%s\"", lv->name);
		if (!deactivate_lv(cmd, lv))
			return_0;
	} else {
		if ((activate == CHANGE_AE) ||
		    lv_is_origin(lv) ||
		    lv_is_thin_type(lv)) {
			log_verbose("Activating logical volume \"%s\" "
				    "exclusively", lv->name);
			if (!activate_lv_excl(cmd, lv))
				return_0;
		} else if (activate == CHANGE_ALY) {
			log_verbose("Activating logical volume \"%s\" locally",
				    lv->name);
			if (!activate_lv_local(cmd, lv))
				return_0;
		} else {
			log_verbose("Activating logical volume \"%s\"",
				    lv->name);
			if (!activate_lv(cmd, lv))
				return_0;
		}

		if (background_polling())
			lv_spawn_background_polling(cmd, lv);
	}

	return 1;
}

static int lvchange_refresh(struct cmd_context *cmd, struct logical_volume *lv)
{
	log_verbose("Refreshing logical volume \"%s\" (if active)", lv->name);

	return lv_refresh(cmd, lv);
}

static int detach_metadata_devices(struct lv_segment *seg, struct dm_list *list)
{
	uint32_t s;
	uint32_t num_meta_lvs;
	struct cmd_context *cmd = seg->lv->vg->cmd;
	struct lv_list *lvl;

	num_meta_lvs = seg_is_raid(seg) ? seg->area_count : !!seg->log_lv;

	if (!num_meta_lvs)
		return_0;

	if (!(lvl = dm_pool_alloc(cmd->mem, sizeof(*lvl) * num_meta_lvs)))
		return_0;

	if (seg_is_raid(seg)) {
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
	struct cmd_context *cmd = seg->lv->vg->cmd;
	struct lv_list *lvl, *tmp;

	if (seg_is_raid(seg)) {
		dm_list_iterate_items_safe(lvl, tmp, list) {
			lv_set_hidden(lvl->lv);
			dm_pool_free(cmd->mem, lvl);
		}
		return 1;
	}

	dm_list_iterate_items(lvl, list)
		break;  /* get first item */

	if (!attach_mirror_log(seg, lvl->lv)) {
		dm_pool_free(cmd->mem, lvl);
		return_0;
	}

	dm_pool_free(cmd->mem, lvl);

	return 1;
}

static int lvchange_resync(struct cmd_context *cmd,
			      struct logical_volume *lv)
{
	int active = 0;
	int monitored;
	struct lvinfo info;
	struct lv_segment *seg = first_seg(lv);
	struct dm_list device_list;
	struct lv_list *lvl;

	dm_list_init(&device_list);

	if (!(lv->status & MIRRORED) && !seg_is_raid(seg)) {
		log_error("Unable to resync %s.  It is not RAID or mirrored.",
			  lv->name);
		return 0;
	}

	if (lv->status & PVMOVE) {
		log_error("Unable to resync pvmove volume %s", lv->name);
		return 0;
	}

	if (lv->status & LOCKED) {
		log_error("Unable to resync locked volume %s", lv->name);
		return 0;
	}

	if (lv_info(cmd, lv, 0, &info, 1, 0)) {
		if (info.open_count) {
			log_error("Can't resync open logical volume \"%s\"",
				  lv->name);
			return 0;
		}

		if (info.exists) {
			if (!arg_count(cmd, yes_ARG) &&
			    yes_no_prompt("Do you really want to deactivate "
					  "logical volume %s to resync it? [y/n]: ",
					  lv->name) == 'n') {
				log_error("Logical volume \"%s\" not resynced",
					  lv->name);
				return 0;
			}

			if (sigint_caught())
				return 0;

			active = 1;
		}
	}

	/* Activate exclusively to ensure no nodes still have LV active */
	monitored = dmeventd_monitor_mode();
	if (monitored != DMEVENTD_MONITOR_IGNORE)
		init_dmeventd_monitor(0);

	if (!deactivate_lv(cmd, lv)) {
		log_error("Unable to deactivate %s for resync", lv->name);
		return 0;
	}

	if (vg_is_clustered(lv->vg) && lv_is_active(lv)) {
		log_error("Can't get exclusive access to clustered volume %s",
			  lv->name);
		return 0;
	}

	if (monitored != DMEVENTD_MONITOR_IGNORE)
		init_dmeventd_monitor(monitored);
	init_mirror_in_sync(0);

	log_very_verbose("Starting resync of %s%s%s%s \"%s\"",
			 (active) ? "active " : "",
			 vg_is_clustered(lv->vg) ? "clustered " : "",
			 (seg->log_lv) ? "disk-logged " :
			 seg_is_raid(seg) ? "" : "core-logged ",
			 seg->segtype->ops->name(seg), lv->name);

	/*
	 * If this mirror has a core log (i.e. !seg->log_lv),
	 * then simply deactivating/activating will cause
	 * it to reset the sync status.  We only need to
	 * worry about persistent logs.
	 */
	if (!seg_is_raid(seg) && !seg->log_lv) {
		if (lv->status & LV_NOTSYNCED) {
			lv->status &= ~LV_NOTSYNCED;
			log_very_verbose("Updating logical volume \"%s\""
					 " on disk(s)", lv->name);
			if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
				log_error("Failed to update metadata on disk.");
				return 0;
			}
		}

		if (active && !activate_lv(cmd, lv)) {
			log_error("Failed to reactivate %s to resynchronize "
				  "mirror", lv->name);
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
		log_error("Failed to clear %s %s for %s",
			  seg->segtype->name, seg_is_raid(seg) ?
			  "metadata area" : "mirror log", lv->name);
		return 0;
	}

	if (!vg_write(lv->vg)) {
		log_error("Failed to write intermediate VG metadata.");
		if (!attach_metadata_devices(seg, &device_list))
			stack;
		if (active && !activate_lv(cmd, lv))
			stack;
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit intermediate VG metadata.");
		if (!attach_metadata_devices(seg, &device_list))
			stack;
		if (active && !activate_lv(cmd, lv))
			stack;
		return 0;
	}

	backup(lv->vg);

	dm_list_iterate_items(lvl, &device_list) {
		if (!activate_lv(cmd, lvl->lv)) {
			log_error("Unable to activate %s for mirror log resync",
				  lvl->lv->name);
			return 0;
		}

		log_very_verbose("Clearing %s device %s",
				 (seg_is_raid(seg)) ? "metadata" : "log",
				 lvl->lv->name);
		if (!set_lv(cmd, lvl->lv, lvl->lv->size, 0)) {
			log_error("Unable to reset sync status for %s",
				  lv->name);
			if (!deactivate_lv(cmd, lvl->lv))
				log_error("Failed to deactivate log LV after "
					  "wiping failed");
			return 0;
		}

		if (!deactivate_lv(cmd, lvl->lv)) {
			log_error("Unable to deactivate %s LV %s "
				  "after wiping for resync",
				  (seg_is_raid(seg)) ? "metadata" : "log",
				  lvl->lv->name);
			return 0;
		}
	}

	/* Put metadata sub-LVs back in place */
	if (!attach_metadata_devices(seg, &device_list)) {
		log_error("Failed to reattach %s device after clearing",
			  (seg_is_raid(seg)) ? "metadata" : "log");
		return 0;
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
		log_error("Failed to update metadata on disk.");
		return 0;
	}

	if (active && !activate_lv(cmd, lv)) {
		log_error("Failed to reactivate %s after resync", lv->name);
		return 0;
	}

	return 1;
}

static int lvchange_alloc(struct cmd_context *cmd, struct logical_volume *lv)
{
	int want_contiguous = 0;
	alloc_policy_t alloc;

	want_contiguous = strcmp(arg_str_value(cmd, contiguous_ARG, "n"), "n");
	alloc = want_contiguous ? ALLOC_CONTIGUOUS : ALLOC_INHERIT;
	alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, alloc);

	if (alloc == lv->alloc) {
		log_error("Allocation policy of logical volume \"%s\" is "
			  "already %s", lv->name, get_alloc_string(alloc));
		return 0;
	}

	lv->alloc = alloc;

	/* FIXME If contiguous, check existing extents already are */

	log_verbose("Setting contiguous allocation policy for \"%s\" to %s",
		    lv->name, get_alloc_string(alloc));

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	/* No need to suspend LV for this change */
	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	return 1;
}

static int lvchange_readahead(struct cmd_context *cmd,
			      struct logical_volume *lv)
{
	unsigned read_ahead = 0;
	unsigned pagesize = (unsigned) lvm_getpagesize() >> SECTOR_SHIFT;
	int r = 0;

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
			log_error("Read ahead is already auto for \"%s\"", lv->name);
		else
			log_error("Read ahead is already %u for \"%s\"",
				  read_ahead, lv->name);
		return 0;
	}

	lv->read_ahead = read_ahead;

	log_verbose("Setting read ahead to %u for \"%s\"", read_ahead,
		    lv->name);

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg))
		return_0;

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		goto out;
	}

	if (!vg_commit(lv->vg)) {
		if (!resume_lv(cmd, lv))
			stack;
		goto_out;
	}

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		goto out;
	}

	r = 1;
out:
	backup(lv->vg);
	return r;
}

static int lvchange_persistent(struct cmd_context *cmd,
			       struct logical_volume *lv)
{
	struct lvinfo info;
	int active = 0;
	int32_t major, minor;

	if (!strcmp(arg_str_value(cmd, persistent_ARG, "n"), "n")) {
		if (!(lv->status & FIXED_MINOR)) {
			log_error("Minor number is already not persistent "
				  "for \"%s\"", lv->name);
			return 0;
		}
		lv->status &= ~FIXED_MINOR;
		lv->minor = -1;
		lv->major = -1;
		log_verbose("Disabling persistent device number for \"%s\"",
			    lv->name);
	} else {
		if (!arg_count(cmd, minor_ARG) && lv->minor < 0) {
			log_error("Minor number must be specified with -My");
			return 0;
		}
		if (arg_count(cmd, major_ARG) > 1) {
			log_error("Option -j/--major may not be repeated.");
			return 0;
		}
		if (arg_count(cmd, minor_ARG) > 1) {
			log_error("Option --minor may not be repeated.");
			return 0;
		}
		if (!arg_count(cmd, major_ARG) && lv->major < 0) {
			log_error("Major number must be specified with -My");
			return 0;
		}
		if (lv_info(cmd, lv, 0, &info, 0, 0) && info.exists)
			active = 1;

		major = arg_int_value(cmd, major_ARG, lv->major);
		minor = arg_int_value(cmd, minor_ARG, lv->minor);
		if (!major_minor_valid(cmd, lv->vg->fid->fmt, major, minor))
			return 0;

		if (active && !arg_count(cmd, force_ARG) &&
		    yes_no_prompt("Logical volume %s will be "
				  "deactivated temporarily. "
				  "Continue? [y/n]: ", lv->name) == 'n') {
			log_error("%s device number not changed.",
				  lv->name);
			return 0;
		}

		if (sigint_caught())
			return 0;

		log_verbose("Ensuring %s is inactive.", lv->name);
		if (!deactivate_lv(cmd, lv)) {
			log_error("%s: deactivation failed", lv->name);
			return 0;
		}
		lv->status |= FIXED_MINOR;
		lv->minor = minor;
		lv->major = major;
		log_verbose("Setting persistent device number to (%d, %d) "
			    "for \"%s\"", lv->major, lv->minor, lv->name);

	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	if (active) {
		log_verbose("Re-activating logical volume \"%s\"", lv->name);
		if (!activate_lv(cmd, lv)) {
			log_error("%s: reactivation failed", lv->name);
			return 0;
		}
	}

	return 1;
}

static int lvchange_tag(struct cmd_context *cmd, struct logical_volume *lv, int arg)
{
	if (!change_tag(cmd, NULL, lv, NULL, arg))
		return_0;

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	/* No need to suspend LV for this change */
	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	return 1;
}

static int lvchange_single(struct cmd_context *cmd, struct logical_volume *lv,
			   void *handle __attribute__((unused)))
{
	int doit = 0, docmds = 0;
	int archived = 0;
	struct logical_volume *origin;
	char snaps_msg[128];

	if (!(lv->vg->status & LVM_WRITE) &&
	    (arg_count(cmd, contiguous_ARG) || arg_count(cmd, permission_ARG) ||
	     arg_count(cmd, readahead_ARG) || arg_count(cmd, persistent_ARG) ||
	     arg_count(cmd, discards_ARG) ||
	     arg_count(cmd, zero_ARG) ||
	     arg_count(cmd, alloc_ARG))) {
		log_error("Only -a permitted with read-only volume "
			  "group \"%s\"", lv->vg->name);
		return EINVALID_CMD_LINE;
	}

	if (lv_is_origin(lv) && !lv_is_thin_volume(lv) &&
	    (arg_count(cmd, contiguous_ARG) || arg_count(cmd, permission_ARG) ||
	     arg_count(cmd, readahead_ARG) || arg_count(cmd, persistent_ARG) ||
	     arg_count(cmd, alloc_ARG))) {
		log_error("Can't change logical volume \"%s\" under snapshot",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv_is_cow(lv) && !lv_is_virtual_origin(origin = origin_from_cow(lv)) &&
	    arg_count(cmd, activate_ARG)) {
		if (origin->origin_count < 2)
			snaps_msg[0] = '\0';
		else if (dm_snprintf(snaps_msg, sizeof(snaps_msg),
				     " and %u other snapshot(s)",
				     origin->origin_count - 1) < 0) {
			log_error("Failed to prepare message.");
			return ECMD_FAILED;
		}

		if (!arg_count(cmd, yes_ARG) &&
		    (yes_no_prompt("Change of snapshot %s will also change its"
				   " origin %s%s. Proceed? [y/n]: ", lv->name,
				   origin->name, snaps_msg) == 'n')) {
			log_error("Logical volume %s not changed.", lv->name);
			return ECMD_FAILED;
		}
	}

	if (lv->status & PVMOVE) {
		log_error("Unable to change pvmove LV %s", lv->name);
		if (arg_count(cmd, activate_ARG))
			log_error("Use 'pvmove --abort' to abandon a pvmove");
		return ECMD_FAILED;
	}

	if (lv->status & MIRROR_LOG) {
		log_error("Unable to change mirror log LV %s directly", lv->name);
		return ECMD_FAILED;
	}

	if (lv->status & MIRROR_IMAGE) {
		log_error("Unable to change mirror image LV %s directly",
			  lv->name);
		return ECMD_FAILED;
	}

	/* If LV is sparse, activate origin instead */
	if (arg_count(cmd, activate_ARG) && lv_is_cow(lv) &&
	    lv_is_virtual_origin(origin = origin_from_cow(lv)))
		lv = origin;

	if (!(lv_is_visible(lv)) && !lv_is_virtual_origin(lv)) {
		log_error("Unable to change internal LV %s directly",
			  lv->name);
		return ECMD_FAILED;
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
	init_background_polling(arg_count(cmd, sysinit_ARG) ? 0 :
						arg_int_value(cmd, poll_ARG,
						DEFAULT_BACKGROUND_POLLING));

	/* access permission change */
	if (arg_count(cmd, permission_ARG)) {
		if (!archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		archived = 1;
		doit += lvchange_permission(cmd, lv);
		docmds++;
	}

	/* allocation policy change */
	if (arg_count(cmd, contiguous_ARG) || arg_count(cmd, alloc_ARG)) {
		if (!archived && !archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		archived = 1;
		doit += lvchange_alloc(cmd, lv);
		docmds++;
	}

	/* read ahead sector change */
	if (arg_count(cmd, readahead_ARG)) {
		if (!archived && !archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		archived = 1;
		doit += lvchange_readahead(cmd, lv);
		docmds++;
	}

	/* persistent device number change */
	if (arg_count(cmd, persistent_ARG)) {
		if (!archived && !archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		archived = 1;
		doit += lvchange_persistent(cmd, lv);
		docmds++;
		if (sigint_caught()) {
			stack;
			return ECMD_FAILED;
		}
	}

	if (arg_count(cmd, discards_ARG) ||
	    arg_count(cmd, zero_ARG)) {
		if (!archived && !archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		archived = 1;
		doit += lvchange_pool_update(cmd, lv);
		docmds++;
	}

	/* add tag */
	if (arg_count(cmd, addtag_ARG)) {
		if (!archived && !archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		archived = 1;
		doit += lvchange_tag(cmd, lv, addtag_ARG);
		docmds++;
	}

	/* del tag */
	if (arg_count(cmd, deltag_ARG)) {
		if (!archived && !archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		archived = 1;
		doit += lvchange_tag(cmd, lv, deltag_ARG);
		docmds++;
	}

	if (doit)
		log_print_unless_silent("Logical volume \"%s\" changed", lv->name);

	if (arg_count(cmd, resync_ARG))
		if (!lvchange_resync(cmd, lv)) {
			stack;
			return ECMD_FAILED;
		}

	/* activation change */
	if (arg_count(cmd, activate_ARG)) {
		if (!_lvchange_activate(cmd, lv)) {
			stack;
			return ECMD_FAILED;
		}
	}

	if (arg_count(cmd, refresh_ARG))
		if (!lvchange_refresh(cmd, lv)) {
			stack;
			return ECMD_FAILED;
		}

	if (!arg_count(cmd, activate_ARG) &&
	    !arg_count(cmd, refresh_ARG) &&
	    arg_count(cmd, monitor_ARG)) {
		if (!lvchange_monitoring(cmd, lv)) {
			stack;
			return ECMD_FAILED;
		}
	}

	if (!arg_count(cmd, activate_ARG) &&
	    !arg_count(cmd, refresh_ARG) &&
	    arg_count(cmd, poll_ARG)) {
		if (!lvchange_background_polling(cmd, lv)) {
			stack;
			return ECMD_FAILED;
		}
	}

	if (doit != docmds) {
		stack;
		return ECMD_FAILED;
	}

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
		arg_count(cmd, contiguous_ARG) ||
		arg_count(cmd, permission_ARG) ||
		arg_count(cmd, readahead_ARG) ||
		arg_count(cmd, persistent_ARG) ||
		arg_count(cmd, addtag_ARG) ||
		arg_count(cmd, deltag_ARG);
	int update_partial_unsafe =
		arg_count(cmd, resync_ARG) ||
		arg_count(cmd, alloc_ARG) ||
		arg_count(cmd, discards_ARG) ||
		arg_count(cmd, zero_ARG);
	int update = update_partial_safe || update_partial_unsafe;

	if (!update &&
            !arg_count(cmd, activate_ARG) && !arg_count(cmd, refresh_ARG) &&
            !arg_count(cmd, monitor_ARG) && !arg_count(cmd, poll_ARG)) {
		log_error("Need 1 or more of -a, -C, -M, -p, -r, -Z, "
			  "--resync, --refresh, --alloc, --addtag, --deltag, "
			  "--monitor, --poll or --discards");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, activate_ARG) && arg_count(cmd, refresh_ARG)) {
		log_error("Only one of -a and --refresh permitted.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_count(cmd, ignorelockingfailure_ARG) ||
	     arg_count(cmd, sysinit_ARG)) && update) {
		log_error("Only -a permitted with --ignorelockingfailure and --sysinit");
		return EINVALID_CMD_LINE;
	}

	if (!update || !update_partial_unsafe)
		cmd->handles_missing_pvs = 1;

	if (!argc) {
		log_error("Please give logical volume path(s)");
		return EINVALID_CMD_LINE;
	}

	if ((arg_count(cmd, minor_ARG) || arg_count(cmd, major_ARG)) &&
	    !arg_count(cmd, persistent_ARG)) {
		log_error("--major and --minor require -My");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, minor_ARG) && argc != 1) {
		log_error("Only give one logical volume when specifying minor");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, contiguous_ARG) && arg_count(cmd, alloc_ARG)) {
		log_error("Only one of --alloc and --contiguous permitted");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, poll_ARG) && arg_count(cmd, sysinit_ARG)) {
		log_error("Only one of --poll and --sysinit permitted");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, sysinit_ARG) && lvmetad_active() &&
	    arg_uint_value(cmd, activate_ARG, 0) == CHANGE_AAY) {
		log_warn("lvmetad is active while using --sysinit -a ay, "
			 "skipping manual activation");
		return ECMD_PROCESSED;
	}

	return process_each_lv(cmd, argc, argv,
			       update ? READ_FOR_UPDATE : 0, NULL,
			       &lvchange_single);
}
