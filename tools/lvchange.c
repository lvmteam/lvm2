/*
 * Copyright (C) 2001  Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
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

	if (!lock_vol(cmd, lv->lvid.s, LCK_LV_SUSPEND | LCK_HOLD)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		unlock_lv(cmd, lv->lvid.s);
		return 0;
	}

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!unlock_lv(cmd, lv->lvid.s)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}

	return 1;
}

static int lvchange_availability(struct cmd_context *cmd,
				 struct logical_volume *lv)
{
	int activate = 0;
	struct physical_volume *pv;

	if (strcmp(arg_str_value(cmd, available_ARG, "n"), "n"))
		activate = 1;

	if (activate) {
		/* FIXME Tighter locking if lv_is_origin() */
		log_verbose("Activating logical volume \"%s\"", lv->name);
		if (!lock_vol(cmd, lv->lvid.s, LCK_LV_ACTIVATE))
			return 0;
		if ((lv->status & LOCKED) && (pv = get_pvmove_pv_from_lv(lv))) {
			log_verbose("Spawning background pvmove process for %s",
				    dev_name(pv->dev));
			pvmove_poll(cmd, dev_name(pv->dev), 1);
		}
	} else {
		log_verbose("Deactivating logical volume \"%s\"", lv->name);
		if (!lock_vol(cmd, lv->lvid.s, LCK_LV_DEACTIVATE))
			return 0;
	}

	return 1;
}

static int lvchange_contiguous(struct cmd_context *cmd,
			       struct logical_volume *lv)
{
	int want_contiguous = 0;

	if (strcmp(arg_str_value(cmd, contiguous_ARG, "n"), "n"))
		want_contiguous = 1;

	if (want_contiguous && lv->alloc == ALLOC_CONTIGUOUS) {
		log_error("Allocation policy of logical volume \"%s\" is "
			  "already contiguous", lv->name);
		return 0;
	}

	if (!want_contiguous && lv->alloc != ALLOC_CONTIGUOUS) {
		log_error
		    ("Allocation policy of logical volume \"%s\" is already"
		     " not contiguous", lv->name);
		return 0;
	}

/******** FIXME lv_check_contiguous?
	if (want_contiguous)
		    && (ret = lv_check_contiguous(vg, lv_index + 1)) == FALSE) {
			log_error("No contiguous logical volume \"%s\"", lv->name);
			return 0;
*********/

	if (want_contiguous) {
		lv->alloc = ALLOC_CONTIGUOUS;
		log_verbose("Setting contiguous allocation policy for \"%s\"",
			    lv->name);
	} else {
		lv->alloc = ALLOC_DEFAULT;
		log_verbose("Reverting to default allocation policy for \"%s\"",
			    lv->name);
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg)) {
		stack;
		return 0;
	}

	backup(lv->vg);

	if (!lock_vol(cmd, lv->lvid.s, LCK_LV_SUSPEND | LCK_HOLD)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		unlock_lv(cmd, lv->lvid.s);
		return 0;
	}

	if (!unlock_lv(cmd, lv->lvid.s)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}

	return 1;

}

static int lvchange_readahead(struct cmd_context *cmd,
			      struct logical_volume *lv)
{
	unsigned int read_ahead = 0;

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

	if (!lock_vol(cmd, lv->lvid.s, LCK_LV_SUSPEND | LCK_HOLD)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		unlock_lv(cmd, lv->lvid.s);
		return 0;
	}

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!unlock_lv(cmd, lv->lvid.s)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}

	return 1;
}

static int lvchange_persistent(struct cmd_context *cmd,
			       struct logical_volume *lv)
{
	struct lvinfo info;

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
		if (lv_info(lv, &info) && info.exists && 
			!arg_count(cmd, force_ARG)) {
			if (yes_no_prompt("Logical volume %s will be "
					  "deactivated first. "
					  "Continue? [y/n]: ",
					  lv->name) == 'n') {
				log_print("%s device number not changed.",
					  lv->name);
				return 0;
			}
		}
		log_print("Ensuring %s is inactive. "
			  "(Reactivate using lvchange -ay.)", lv->name);
		if (!lock_vol(cmd, lv->lvid.s, LCK_LV_DEACTIVATE)) {
			log_error("%s: deactivation failed", lv->name);
			return 0;
		}
		lv->status |= FIXED_MINOR;
		lv->minor = arg_int_value(cmd, minor_ARG, lv->minor);
		lv->major = arg_int_value(cmd, major_ARG, lv->major);
		log_verbose("Setting persistent device number to (%d, %d) "
			    "for \"%s\"", lv->major, lv->minor, lv->name);
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!vg_write(lv->vg)) {
		stack;
		return 0;
	}

	backup(lv->vg);

	if (!lock_vol(cmd, lv->lvid.s, LCK_LV_SUSPEND | LCK_HOLD)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		unlock_lv(cmd, lv->lvid.s);
		return 0;
	}

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!unlock_lv(cmd, lv->lvid.s)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}

	return 1;
}

static int lvchange_single(struct cmd_context *cmd, struct logical_volume *lv,
			   void *handle)
{
	int doit = 0;
	int archived = 0;

	if (!(lv->vg->status & LVM_WRITE) &&
	    (arg_count(cmd, contiguous_ARG) || arg_count(cmd, permission_ARG) ||
	     arg_count(cmd, readahead_ARG) || arg_count(cmd, persistent_ARG))) {
		log_error("Only -a permitted with read-only volume "
			  "group \"%s\"", lv->vg->name);
		return EINVALID_CMD_LINE;
	}

	if (lv_is_origin(lv) &&
	    (arg_count(cmd, contiguous_ARG) || arg_count(cmd, permission_ARG) ||
	     arg_count(cmd, readahead_ARG) || arg_count(cmd, persistent_ARG))) {
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

	/* access permission change */
	if (arg_count(cmd, permission_ARG)) {
		if (!archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_permission(cmd, lv);
	}

	/* allocation policy change */
	if (arg_count(cmd, contiguous_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_contiguous(cmd, lv);
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

	if (doit)
		log_print("Logical volume \"%s\" changed", lv->name);

	/* availability change */
	if (arg_count(cmd, available_ARG))
		if (!lvchange_availability(cmd, lv))
			return ECMD_FAILED;

	return 0;
}

int lvchange(struct cmd_context *cmd, int argc, char **argv)
{
	if (!arg_count(cmd, available_ARG) && !arg_count(cmd, contiguous_ARG)
	    && !arg_count(cmd, permission_ARG) && !arg_count(cmd, readahead_ARG)
	    && !arg_count(cmd, minor_ARG) && !arg_count(cmd, major_ARG)
	    && !arg_count(cmd, persistent_ARG)) {
		log_error("One or more of -a, -C, -j, -m, -M, -p or -r "
			  "required");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, ignorelockingfailure_ARG) &&
	    (arg_count(cmd, contiguous_ARG) || arg_count(cmd, permission_ARG) ||
	     arg_count(cmd, readahead_ARG) || arg_count(cmd, persistent_ARG))) {
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

	return process_each_lv(cmd, argc, argv, LCK_VG_WRITE, NULL,
			       &lvchange_single);
}
