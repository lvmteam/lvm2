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

static int lvchange_single(struct logical_volume *lv);
static int lvchange_permission(struct logical_volume *lv);
static int lvchange_availability(struct logical_volume *lv);
static int lvchange_contiguous(struct logical_volume *lv);
static int lvchange_readahead(struct logical_volume *lv);
static int lvchange_persistent(struct logical_volume *lv);

int lvchange(int argc, char **argv)
{
	if (!arg_count(available_ARG) && !arg_count(contiguous_ARG)
	    && !arg_count(permission_ARG) && !arg_count(readahead_ARG)
	    && !arg_count(minor_ARG) && !arg_count(persistent_ARG)) {
		log_error("One or more of -a, -C, -m, -M, -p or -r required");
		return EINVALID_CMD_LINE;
	}

	if (!argc) {
		log_error("Please give logical volume path(s)");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(minor_ARG) && argc != 1) {
		log_error("Only give one logical volume when specifying minor");
		return EINVALID_CMD_LINE;
	}

	return process_each_lv(argc, argv, &lvchange_single);
}

static int lvchange_single(struct logical_volume *lv)
{
	int doit = 0;
	int archived = 0;

	if (!(lv->vg->status & LVM_WRITE) && 
	    (arg_count(contiguous_ARG) || arg_count(permission_ARG) ||
	     arg_count(readahead_ARG) || arg_count(persistent_ARG))) {
		log_error("Only -a permitted with read-only volume "
			  "group \"%s\"", lv->vg->name);
		return EINVALID_CMD_LINE;
	}

	if (lv->status & SNAPSHOT_ORG) {
		log_error("Can't change logical volume \"%s\" under snapshot",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv->status & SNAPSHOT) {
		log_error("Can't change snapshot logical volume \"%s\"", 
			  lv->name);
		return ECMD_FAILED;
	}

	/* access permission change */
	if (arg_count(permission_ARG)) {
		if (!archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_permission(lv);
	}

	/* allocation policy change */
	if (arg_count(contiguous_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_contiguous(lv);
	}

	/* read ahead sector change */
	if (arg_count(readahead_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_readahead(lv);
	}

	/* read ahead sector change */
	if (arg_count(persistent_ARG)) {
		if (!archived && !archive(lv->vg))
			return ECMD_FAILED;
		archived = 1;
		doit += lvchange_persistent(lv);
	}

	if (doit)
		log_print("Logical volume \"%s\" changed", lv->name);

	/* availability change */
	if (arg_count(available_ARG))
		if (!lvchange_availability(lv))
			return ECMD_FAILED;

	return 0;
}

static int lvchange_permission(struct logical_volume *lv)
{
	int lv_access;

	lv_access = arg_int_value(permission_ARG, 0);

	if ((lv_access & LVM_WRITE) && (lv->status & LVM_WRITE)) {
		log_error("Logical volume \"%s\" is already writable", lv->name);
		return 0;
	}

	if (!(lv_access & LVM_WRITE) && !(lv->status & LVM_WRITE)) {
		log_error("Logical volume \"%s\" is already read only", lv->name);
		return 0;
	}

	if (lv_access & LVM_WRITE) {
		lv->status |= LVM_WRITE;
		log_verbose("Setting logical volume \"%s\" read/write", lv->name);
	} else {
		lv->status &= ~LVM_WRITE;
		log_verbose("Setting logical volume \"%s\" read-only", lv->name);
	}


	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);
	if (!fid->ops->vg_write(fid, lv->vg))
		return 0;

	backup(lv->vg);

	log_very_verbose("Updating permissions for \"%s\" in kernel", lv->name);
	if (!lv_update_write_access(lv))
		return 0;

	return 1;
}

static int lvchange_availability(struct logical_volume *lv)
{
	int activate = 0;
	int active;

	if (strcmp(arg_str_value(available_ARG, "n"), "n"))
		activate = 1;

	if (arg_count(minor_ARG)) {
		lv->minor = arg_int_value(minor_ARG, -1);
	}

	if ((active = lv_active(lv)) < 0) {
		log_error("Unable to determine status of \"%s\"", lv->name);
		return 0;
	}

	if (activate && active) {
		log_verbose("Logical volume \"%s\" is already active", 
			    lv->name);
		return 0;
	}

	if (!activate && !active) {
		log_verbose("Logical volume \"%s\" is already inactive", 
			    lv->name);
		return 0;
	}
	
	if (activate & lv_suspended(lv)) {
		log_verbose("Reactivating logical volume \"%s\"", lv->name); 
		if (!lv_reactivate(lv))
			return 0;
		return 1;
	}
	
	if (activate) {
		log_verbose("Activating logical volume \"%s\"", lv->name);
		if (!lv_activate(lv))
			return 0;
	} else {
		log_verbose("Deactivating logical volume \"%s\"", lv->name);
		if (!lv_deactivate(lv))
			return 0;
	}

	return 1;
}

static int lvchange_contiguous(struct logical_volume *lv)
{
	int lv_allocation = 0;

	if (strcmp(arg_str_value(contiguous_ARG, "n"), "n"))
		lv_allocation |= ALLOC_CONTIGUOUS;

	if ((lv_allocation & ALLOC_CONTIGUOUS) &&
	    (lv->status & ALLOC_CONTIGUOUS)) {
		log_error("Allocation policy of logical volume \"%s\" is "
			  "already contiguous", lv->name);
		return 0;
	}

	if (!(lv_allocation & ALLOC_CONTIGUOUS) &&
	    !(lv->status & ALLOC_CONTIGUOUS)) {
		log_error("Allocation policy of logical volume \"%s\" is already"
			  " not contiguous", lv->name);
		return 0;
	}

/******** FIXME lv_check_contiguous? 
	if ((lv_allocation & ALLOC_CONTIGUOUS)
		    && (ret = lv_check_contiguous(vg, lv_index + 1)) == FALSE) {
			log_error("No contiguous logical volume \"%s\"", lv->name);
			return 0;
*********/

	if (lv_allocation & ALLOC_CONTIGUOUS) {
		lv->status |= ALLOC_CONTIGUOUS;
		log_verbose("Setting contiguous allocation policy for \"%s\"",
			    lv->name);
	} else {
		lv->status &= ~ALLOC_CONTIGUOUS;
		log_verbose("Removing contiguous allocation policy for \"%s\"",
			    lv->name);
	}

	log_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	if (!fid->ops->vg_write(fid, lv->vg))
		return 0;

	backup(lv->vg);

	return 1;
}

static int lvchange_readahead(struct logical_volume *lv)
{
	int read_ahead = 0;

	read_ahead = arg_int_value(readahead_ARG, 0);

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
	log_verbose("Setting read ahead to %u for \"%s\"", read_ahead, lv->name);

	log_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	if (!fid->ops->vg_write(fid, lv->vg))
		return 0;

	backup(lv->vg);

	return 1;
}

static int lvchange_persistent(struct logical_volume *lv)
{
	if (!strcmp(arg_str_value(persistent_ARG, "n"), "n")) {
		if (!(lv->status & FIXED_MINOR)) {
			log_error("Minor number is already not persistent "
				  "for \"%s\"", lv->name);
			return 0;
		}
		lv->status &= ~FIXED_MINOR;
		lv->minor = -1;
		log_verbose("Disabling persistent minor for \"%s\"", lv->name);
	} else {
		if (lv_active(lv)) {
			log_error("Cannot change minor number when active");
			return 0;
		}
		if (!arg_count(minor_ARG)) {
			log_error("Minor number must be specified with -My");
			return 0;
		}
		lv->status |= FIXED_MINOR;
		lv->minor = arg_int_value(minor_ARG, -1);
		log_verbose("Setting persistent minor number to %d for \"%s\"",
			    lv->minor, lv->name);
	}

	log_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	if (!fid->ops->vg_write(fid, lv->vg))
		return 0;

	backup(lv->vg);

	return 1;
}

