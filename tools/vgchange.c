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

static int _activate_lvs_in_vg(struct cmd_context *cmd,
			       struct volume_group *vg, int lock)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct physical_volume *pv;
	int count = 0;

	list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		/* Only request activation of snapshot origin devices */
		if (lv_is_cow(lv))
			continue;

		/* Can't deactive a pvmove LV */
		if ((lock == LCK_LV_DEACTIVATE) && (lv->status & PVMOVE))
			continue;

		if (!lock_vol(cmd, lv->lvid.s, lock | LCK_NONBLOCK))
			continue;

		if ((lv->status & PVMOVE) &&
		    (pv = get_pvmove_pv_from_lv_mirr(lv))) {
			log_verbose("Spawning background process for %s %s",
				    lv->name, dev_name(pv->dev));
			pvmove_poll(cmd, dev_name(pv->dev), 1);
			continue;
		}

		count++;
	}

	return count;
}

static void _vgchange_available(struct cmd_context *cmd,
				struct volume_group *vg)
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

static void _vgchange_resizeable(struct cmd_context *cmd,
				 struct volume_group *vg)
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

	if (!vg_write(vg) || !vg_commit(vg))
		return;

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return;
}

static void _vgchange_logicalvolume(struct cmd_context *cmd,
				    struct volume_group *vg)
{
	uint32_t max_lv = arg_uint_value(cmd, logicalvolume_ARG, 0);

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

	if (!vg_write(vg) || !vg_commit(vg))
		return;

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return;
}

static int vgchange_single(struct cmd_context *cmd, const char *vg_name,
			   struct volume_group *vg, int consistent,
			   void *handle)
{
	if (!vg) {
		log_error("Unable to find volume group \"%s\"", vg_name);
		return ECMD_FAILED;
	}

	if (!consistent) {
		unlock_vg(cmd, vg_name);
		log_error("Volume group \"%s\" inconsistent", vg_name);
		if (!(vg = recover_vg(cmd, vg_name, LCK_VG_WRITE)))
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
		_vgchange_available(cmd, vg);

	if (arg_count(cmd, resizeable_ARG))
		_vgchange_resizeable(cmd, vg);

	if (arg_count(cmd, logicalvolume_ARG))
		_vgchange_logicalvolume(cmd, vg);

	return 0;
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
			       LCK_VG_READ : LCK_VG_WRITE, 0, NULL,
			       &vgchange_single);
}
