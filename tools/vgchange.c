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

static int vgchange_single(struct cmd_context *cmd, const char *vg_name);
void vgchange_available(struct cmd_context *cmd, struct volume_group *vg);
void vgchange_resizeable(struct cmd_context *cmd, struct volume_group *vg);
void vgchange_logicalvolume(struct cmd_context *cmd, struct volume_group *vg);

static int _activate_lvs_in_vg(struct cmd_context *cmd,
			       struct volume_group *vg, int lock)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		/* Only request activatation of snapshot origin devices */
		if (lv_is_cow(lv))
			continue;

		if (!lock_vol(cmd, lv->lvid.s, lock | LCK_NONBLOCK))
			continue;

		count++;
	}

	return count;
}

int vgchange(struct cmd_context *cmd, int argc, char **argv)
{
	if (!
	    (arg_count(cmd, available_ARG) + arg_count(cmd, logicalvolume_ARG) +
	     arg_count(cmd, resizeable_ARG))) {
		log_error("One of -a, -l or -x options required");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, available_ARG) + arg_count(cmd, logicalvolume_ARG) +
	    arg_count(cmd, resizeable_ARG) > 1) {
		log_error("Only one of -a, -l or -x options allowed");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, ignorelockingfailure_ARG) &&
	    !arg_count(cmd, available_ARG)) {
		log_error("--ignorelockingfailure only available with -a");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, available_ARG) == 1
	    && arg_count(cmd, autobackup_ARG)) {
		log_error("-A option not necessary with -a option");
		return EINVALID_CMD_LINE;
	}

	return process_each_vg(cmd, argc, argv,
			       (arg_count(cmd, available_ARG)) ?
			       LCK_VG_READ : LCK_VG_WRITE, &vgchange_single);
}

static int vgchange_single(struct cmd_context *cmd, const char *vg_name)
{
	struct volume_group *vg;

	if (!(vg = vg_read(cmd, vg_name))) {
		log_error("Unable to find volume group \"%s\"", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg->status & LVM_WRITE) && !arg_count(cmd, available_ARG)) {
		log_error("Volume group \"%s\" is read-only", vg->name);
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg_name);
		return ECMD_FAILED;
	}

	if (arg_count(cmd, available_ARG))
		vgchange_available(cmd, vg);

	if (arg_count(cmd, resizeable_ARG))
		vgchange_resizeable(cmd, vg);

	if (arg_count(cmd, logicalvolume_ARG))
		vgchange_logicalvolume(cmd, vg);

	return 0;
}

void vgchange_available(struct cmd_context *cmd, struct volume_group *vg)
{
	int lv_open, active;
	int available = !strcmp(arg_str_value(cmd, available_ARG, "n"), "y");

	/* FIXME: Force argument to deactivate them? */
	if (!available && (lv_open = lvs_in_vg_opened(vg))) {
		log_error("Can't deactivate volume group \"%s\" with %d open "
			  "logical volume(s)", vg->name, lv_open);
		return;
	}

	if (available && (active = lvs_in_vg_activated(vg)))
		log_verbose("%d logical volume(s) in volume group \"%s\" "
			    "already active", active, vg->name);

	if (available && _activate_lvs_in_vg(cmd, vg, LCK_LV_ACTIVATE))
		log_verbose("Activated logical volumes in "
			    "volume group \"%s\"", vg->name);

	if (!available && _activate_lvs_in_vg(cmd, vg, LCK_LV_DEACTIVATE))
		log_verbose("Deactivated logical volumes in "
			    "volume group \"%s\"", vg->name);

	log_print("%d logical volume(s) in volume group \"%s\" now active",
		  lvs_in_vg_activated(vg), vg->name);
	return;
}

void vgchange_resizeable(struct cmd_context *cmd, struct volume_group *vg)
{
	int resizeable = !strcmp(arg_str_value(cmd, resizeable_ARG, "n"), "y");

	if (resizeable && (vg->status & RESIZEABLE_VG)) {
		log_error("Volume group \"%s\" is already resizeable",
			  vg->name);
		return;
	}

	if (!resizeable && !(vg->status & RESIZEABLE_VG)) {
		log_error("Volume group \"%s\" is already not resizeable",
			  vg->name);
		return;
	}

	if (!archive(vg))
		return;

	if (resizeable)
		vg->status |= RESIZEABLE_VG;
	else
		vg->status &= ~RESIZEABLE_VG;

	if (!vg_write(vg))
		return;

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return;
}

void vgchange_logicalvolume(struct cmd_context *cmd, struct volume_group *vg)
{
	int max_lv = arg_int_value(cmd, logicalvolume_ARG, 0);

	if (!(vg->status & RESIZEABLE_VG)) {
		log_error("Volume group \"%s\" must be resizeable "
			  "to change MaxLogicalVolume", vg->name);
		return;
	}

	if (max_lv < vg->lv_count) {
		log_error("MaxLogicalVolume is less than the current number "
			  "%d of logical volume(s) for \"%s\"", vg->lv_count,
			  vg->name);
		return;
	}

	if (!archive(vg))
		return;

	vg->max_lv = max_lv;

	if (!vg_write(vg))
		return;

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return;
}
