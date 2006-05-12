/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved. 
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
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

#include "tools.h"

static int lvchange_permission(struct cmd_context *cmd,
			       struct logical_volume *lv)
{
	uint32_t lv_access;

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
	if (!vg_write(lv->vg)) {
		stack;
		return 0;
	}

	backup(lv->vg);

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		resume_lv(cmd, lv);
		return 0;
	}

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}

	return 1;
}

static int lvchange_registration(struct cmd_context *cmd,
				 struct logical_volume *lv)
{
	int r;
	struct lvinfo info;

	if (!lv_info(cmd, lv, &info, 0) || !info.exists) {
		log_error("Logical volume, %s, is not active", lv->name);
		return 0;
	}

	/* do not register pvmove lv's */
	if (lv->status & PVMOVE)
		return 1;

	log_verbose("%smonitoring logical volume \"%s\"",
		    (dmeventd_register_mode()) ? "" : "Not ", lv->name);
	r = register_dev_for_events(cmd, lv, dmeventd_register_mode());

	if (r < 0) {
		log_error("Unable to %smonitor logical volume, %s",
			  (dmeventd_register_mode()) ? "" : "un", lv->name);
		r = 0;
	} else if (!r) {
		log_verbose("Logical volume %s needs no monitoring.",
			    lv->name);
		r = 1;
	}

	return r;
}

static int lvchange_availability(struct cmd_context *cmd,
				 struct logical_volume *lv)
{
	int activate;
	const char *pvname;

	activate = arg_uint_value(cmd, available_ARG, 0);

	if (activate == CHANGE_ALN) {
		log_verbose("Deactivating logical volume \"%s\" locally",
			    lv->name);
		if (!deactivate_lv_local(cmd, lv)) {
			stack;
			return 0;
		}
	} else if (activate == CHANGE_AN) {
		log_verbose("Deactivating logical volume \"%s\"", lv->name);
		if (!deactivate_lv(cmd, lv)) {
			stack;
			return 0;
		}
	} else {
		if (lockingfailed() && (lv->vg->status & CLUSTERED)) {
                	log_verbose("Locking failed: ignoring clustered "
				    "logical volume %s", lv->name);
                	return 0;
        	}

		if (lv_is_origin(lv) || (activate == CHANGE_AE)) {
			log_verbose("Activating logical volume \"%s\" "
				    "exclusively", lv->name);
			if (!activate_lv_excl(cmd, lv)) {
				stack;
				return 0;
			}
		} else if (activate == CHANGE_ALY) {
			log_verbose("Activating logical volume \"%s\" locally",
				    lv->name);
			if (!activate_lv_local(cmd, lv)) {
				stack;
				return 0;
			}
		} else {
			log_verbose("Activating logical volume \"%s\"",
				    lv->name);
			if (!activate_lv(cmd, lv)) {
				stack;
				return 0;
			}
		}

		if ((lv->status & LOCKED) &&
		    (pvname = get_pvmove_pvname_from_lv(lv))) {
			log_verbose("Spawning background pvmove process for %s",
				    pvname);
			pvmove_poll(cmd, pvname, 1);
		}
	}

	return 1;
}

static int lvchange_refresh(struct cmd_context *cmd, struct logical_volume *lv)
{
	log_verbose("Refreshing logical volume \"%s\" (if active)", lv->name);
	if (!suspend_lv(cmd, lv) || !resume_lv(cmd, lv))
		return 0;

	return 1;
}

static int lvchange_alloc(struct cmd_context *cmd, struct logical_volume *lv)
{
	int want_contiguous = 0;
	alloc_policy_t alloc;

	want_contiguous = strcmp(arg_str_value(cmd, contiguous_ARG, "n"), "n");
	alloc = want_contiguous ? ALLOC_CONTIGUOUS : ALLOC_INHERIT;
	alloc = arg_uint_value(cmd, alloc_ARG, alloc);

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

	if (!vg_write(lv->vg)) {
		stack;
		return 0;
	}

	backup(lv->vg);

	/* No need to suspend LV for this change */
	if (!vg_commit(lv->vg)) {
		stack;
		return 0;
	}

	return 1;
}

static int lvchange_readahead(struct cmd_context *cmd,
			      struct logical_volume *lv)
{
	unsigned read_ahead = 0;

	read_ahead = arg_uint_value(cmd, readahead_ARG, 0);

/******* FIXME Ranges?
	if (read_ahead < LVM_MIN_READ_AHEAD || read_ahead > LVM_MAX_READ_AHEAD) {
		log_error("read ahead sector argument is invalid");
		return 0;
	}
********/

	if (lv->read_ahead == read_ahead) {
		log_error("Read ahead is already %u for \"%s\"",
			  read_ahead, lv->name);
		return 0;
	}

	lv->read_ahead = read_ahead;

	log_verbose("Setting read ahead to %u for \"%s\"", read_ahead,
		    lv->name);

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg)) {
		stack;
		return 0;
	}

	backup(lv->vg);

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		resume_lv(cmd, lv);
		return 0;
	}

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}

	return 1;
}

static int lvchange_persistent(struct cmd_context *cmd,
			       struct logical_volume *lv)
{
	struct lvinfo info;
	int active = 0;

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
		if (!arg_count(cmd, major_ARG) && lv->major < 0) {
			log_error("Major number must be specified with -My");
			return 0;
		}
		if (lv_info(cmd, lv, &info, 0) && info.exists &&
		    !arg_count(cmd, force_ARG)) {
			if (yes_no_prompt("Logical volume %s will be "
					  "deactivated temporarily. "
					  "Continue? [y/n]: ", lv->name) == 'n') {
				log_print("%s device number not changed.",
					  lv->name);
				return 0;
			}
			active = 1;
		}
		log_verbose("Ensuring %s is inactive.", lv->name);
		if (!deactivate_lv(cmd, lv)) {
			log_error("%s: deactivation failed", lv->name);
			return 0;
		}
		lv->status |= FIXED_MINOR;
		lv->minor = arg_int_value(cmd, minor_ARG, lv->minor);
		lv->major = arg_int_value(cmd, major_ARG, lv->major);
		log_verbose("Setting persistent device number to (%d, %d) "
			    "for \"%s\"", lv->major, lv->minor, lv->name);
		if (active) {
			log_verbose("Re-activating logical volume \"%s\"",
				    lv->name);
			if (!activate_lv(cmd, lv)) {
				log_error("%s: reactivation failed", lv->name);
				return 0;
			}
		}
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg)) {
		stack;
		return 0;
	}

	backup(lv->vg);

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		resume_lv(cmd, lv);
		return 0;
	}

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}

	return 1;
}

static int lvchange_tag(struct cmd_context *cmd, struct logical_volume *lv,
			int arg)
{
	const char *tag;

	if (!(tag = arg_str_value(cmd, arg, NULL))) {
		log_error("Failed to get tag");
		return 0;
	}

	if (!(lv->vg->fid->fmt->features & FMT_TAGS)) {
		log_error("Logical volume %s/%s does not support tags",
			  lv->vg->name, lv->name);
		return 0;
	}

	if ((arg == addtag_ARG)) {
		if (!str_list_add(cmd->mem, &lv->tags, tag)) {
			log_error("Failed to add tag %s to %s/%s",
				  tag, lv->vg->name, lv->name);
			return 0;
		}
	} else {
		if (!str_list_del(&lv->tags, tag)) {
			log_error("Failed to remove tag %s from %s/%s",
				  tag, lv->vg->name, lv->name);
			return 0;
		}
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg)) {
		stack;
		return 0;
	}

	backup(lv->vg);

	/* No need to suspend LV for this change */
	if (!vg_commit(lv->vg)) {
		stack;
		return 0;
	}

	return 1;
}

static int lvchange_single(struct cmd_context *cmd, struct logical_volume *lv,
			   void *handle __attribute((unused)))
{
	int doit = 0;
	int archived = 0;

	if (!(lv->vg->status & LVM_WRITE) &&
	    (arg_count(cmd, contiguous_ARG) || arg_count(cmd, permission_ARG) ||
	     arg_count(cmd, readahead_ARG) || arg_count(cmd, persistent_ARG) ||
	     arg_count(cmd, alloc_ARG))) {
		log_error("Only -a permitted with read-only volume "
			  "group \"%s\"", lv->vg->name);
		return EINVALID_CMD_LINE;
	}

	if (lv_is_origin(lv) &&
	    (arg_count(cmd, contiguous_ARG) || arg_count(cmd, permission_ARG) ||
	     arg_count(cmd, readahead_ARG) || arg_count(cmd, persistent_ARG) ||
	     arg_count(cmd, alloc_ARG))) {
		log_error("Can't change logical volume \"%s\" under snapshot",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv_is_cow(lv)) {
		log_error("Can't change snapshot logical volume \"%s\"",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv->status & PVMOVE) {
		log_error("Unable to change pvmove LV %s", lv->name);
		if (arg_count(cmd, available_ARG))
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

	if (!(lv->status & VISIBLE_LV)) {
		log_error("Unable to change internal LV %s directly",
			  lv->name);
		return ECMD_FAILED;
	}

	init_dmeventd_register(arg_int_value(cmd, monitor_ARG, DEFAULT_DMEVENTD_MONITOR));

	/* access permission change */
	if (arg_count(cmd, permission_ARG)) {
		if (!archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_permission(cmd, lv);
	}

	/* allocation policy change */
	if (arg_count(cmd, contiguous_ARG) || arg_count(cmd, alloc_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_alloc(cmd, lv);
	}

	/* read ahead sector change */
	if (arg_count(cmd, readahead_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_readahead(cmd, lv);
	}

	/* read ahead sector change */
	if (arg_count(cmd, persistent_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_persistent(cmd, lv);
	}

	/* add tag */
	if (arg_count(cmd, addtag_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_tag(cmd, lv, addtag_ARG);
	}

	/* del tag */
	if (arg_count(cmd, deltag_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_tag(cmd, lv, deltag_ARG);
	}

	if (doit)
		log_print("Logical volume \"%s\" changed", lv->name);

	/* availability change */
	if (arg_count(cmd, available_ARG)) {
		if (!lvchange_availability(cmd, lv))
			return ECMD_FAILED;
	}

	if (arg_count(cmd, refresh_ARG))
		if (!lvchange_refresh(cmd, lv))
			return ECMD_FAILED;

	if (!arg_count(cmd, available_ARG) &&
	    !arg_count(cmd, refresh_ARG) &&
	    arg_count(cmd, monitor_ARG)) {
		if (!lvchange_registration(cmd, lv))
			return ECMD_FAILED;
	}

	return ECMD_PROCESSED;
}

int lvchange(struct cmd_context *cmd, int argc, char **argv)
{
	if (!arg_count(cmd, available_ARG) && !arg_count(cmd, contiguous_ARG)
	    && !arg_count(cmd, permission_ARG) && !arg_count(cmd, readahead_ARG)
	    && !arg_count(cmd, minor_ARG) && !arg_count(cmd, major_ARG)
	    && !arg_count(cmd, persistent_ARG) && !arg_count(cmd, addtag_ARG)
	    && !arg_count(cmd, deltag_ARG) && !arg_count(cmd, refresh_ARG)
	    && !arg_count(cmd, alloc_ARG) && !arg_count(cmd, monitor_ARG)) {
		log_error("Need 1 or more of -a, -C, -j, -m, -M, -p, -r, "
			  "--refresh, --alloc, --addtag, --deltag "
			  "or --monitor");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, ignorelockingfailure_ARG) &&
	    (arg_count(cmd, contiguous_ARG) || arg_count(cmd, permission_ARG) ||
	     arg_count(cmd, readahead_ARG) || arg_count(cmd, persistent_ARG) ||
	     arg_count(cmd, addtag_ARG) || arg_count(cmd, deltag_ARG) ||
	     arg_count(cmd, refresh_ARG) || arg_count(cmd, alloc_ARG))) {
		log_error("Only -a permitted with --ignorelockingfailure");
		return EINVALID_CMD_LINE;
	}

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

	return process_each_lv(cmd, argc, argv, LCK_VG_WRITE, NULL,
			       &lvchange_single);
}
