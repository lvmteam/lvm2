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

static int vgchange_single(const char *vg_name);
void vgchange_available(struct volume_group *vg);
void vgchange_resizeable(struct volume_group *vg);
void vgchange_logicalvolume(struct volume_group *vg);

int vgchange(int argc, char **argv)
{
	if (!(arg_count(available_ARG) + arg_count(logicalvolume_ARG) +
	      arg_count(resizeable_ARG))) {
		log_error("One of -a, -l or -x options required");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(available_ARG) + arg_count(logicalvolume_ARG) +
	    arg_count(resizeable_ARG) > 1) {
		log_error("Only one of -a, -l or -x options allowed");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(available_ARG) == 1 && arg_count(autobackup_ARG)) {
		log_error("-A option not necessary with -a option");
		return EINVALID_CMD_LINE;
	}

	return process_each_vg(argc, argv, 
			       (arg_count(available_ARG)) ? 
						LCK_READ : LCK_WRITE,
			       &vgchange_single);
}

static int vgchange_single(const char *vg_name)
{
	struct volume_group *vg;

	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Unable to find volume group \"%s\"", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg->status & LVM_WRITE) && !arg_count(available_ARG)) {
		log_error("Volume group \"%s\" is read-only", vg->name);
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg_name);
		return ECMD_FAILED;
	}

	if (arg_count(available_ARG))
		vgchange_available(vg);

	if (arg_count(resizeable_ARG))
		vgchange_resizeable(vg);

	if (arg_count(logicalvolume_ARG))
		vgchange_logicalvolume(vg);

	return 0;
}

void vgchange_available(struct volume_group *vg)
{
	int lv_open, lv_active;
	int available = !strcmp(arg_str_value(available_ARG, "n"), "y");

	/* FIXME: Force argument to deactivate them? */
	if (!available && (lv_open = lvs_in_vg_opened(vg))) {
		log_error("Can't deactivate volume group \"%s\" with %d open "
			  "logical volume(s)", vg->name, lv_open);
		return;
	}

	if (available && (lv_active = lvs_in_vg_activated(vg)))
		log_verbose("%d logical volume(s) in volume group \"%s\" "
			    "already active", lv_active, vg->name);

	if (available && (lv_open = activate_lvs_in_vg(vg)))
		log_verbose("Activated %d logical volumes in "
			    "volume group \"%s\"", lv_open, vg->name);

	if (!available && (lv_open = deactivate_lvs_in_vg(vg)))
		log_verbose("Deactivated %d logical volumes in "
			    "volume group \"%s\"", lv_open, vg->name);

	log_print("%d logical volume(s) in volume group \"%s\" now active",
		  lvs_in_vg_activated(vg), vg->name);
	return;
}

void vgchange_resizeable(struct volume_group *vg)
{
	int resizeable = !strcmp(arg_str_value(resizeable_ARG, "n"), "y");

	if (resizeable && (vg->status & RESIZEABLE_VG)) {
		log_error("Volume group \"%s\" is already resizeable", vg->name);
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

	if (!fid->ops->vg_write(fid, vg))
		return;

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return;
}

void vgchange_logicalvolume(struct volume_group *vg)
{
	int max_lv = arg_int_value(logicalvolume_ARG, 0);

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

/************** FIXME  To be handled within vg_write
	    for (p = 0, pp = *vg->pv; p < vg->pv_max; p++, pp++) {
		if (pp != NULL) {
		    pp->lv_on_disk.size =
			round_up((max_lv + 1) * sizeof(lv_disk_t), LVM_VGDA_ALIGN);
		    pe_on_disk_base_sav = pp->pe_on_disk.base;
		    pp->pe_on_disk.base = pp->lv_on_disk.base + pp->lv_on_disk.size;
		    pp->pe_on_disk.size -= pp->pe_on_disk.base - pe_on_disk_base_sav;
		    if (LVM_VGDA_SIZE(pp) / SECTOR_SIZE >
			(pp->pv_size & ~LVM_PE_ALIGN) - pp->pe_total * pp->pe_size) {
			log_error("extended VGDA would overlap first physical extent");
			return LVM_E_PE_OVERLAP;
		    }
		}
	    }
****************/

	if (!archive(vg))
		return;

	vg->max_lv = max_lv;

	if (!fid->ops->vg_write(fid, vg))
		return;

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return;
}
