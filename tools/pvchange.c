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

int pvchange_single(struct cmd_context *cmd, struct physical_volume *pv);

int pvchange(struct cmd_context *cmd, int argc, char **argv)
{
	int opt = 0;
	int done = 0;
	int total = 0;

	struct physical_volume *pv;
	char *pv_name;

	struct list *pvh, *pvs;

	if (arg_count(cmd, allocatable_ARG) == 0) {
		log_error("Please give the x option");
		return EINVALID_CMD_LINE;
	}

	if (!(arg_count(cmd, all_ARG)) && !argc) {
		log_error("Please give a physical volume path");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, all_ARG) && argc) {
		log_error("Option a and PhysicalVolumePath are exclusive");
		return EINVALID_CMD_LINE;
	}

	if (argc) {
		log_verbose("Using physical volume(s) on command line");
		for (; opt < argc; opt++) {
			pv_name = argv[opt];
			if (!(pv = pv_read(cmd, pv_name))) {
				log_error
				    ("Failed to read physical volume \"%s\"",
				     pv_name);
				continue;
			}
			total++;
			done += pvchange_single(cmd, pv);
		}
	} else {
		log_verbose("Scanning for physical volume names");
		if (!(pvs = get_pvs(cmd))) {
			return ECMD_FAILED;
		}

		list_iterate(pvh, pvs) {
			total++;
			done += pvchange_single(cmd,
						list_item(pvh,
							  struct pv_list)->pv);
		}
	}

	log_print("%d physical volume%s changed / %d physical volume%s "
		  "not changed",
		  done, done > 1 ? "s" : "",
		  total - done, total - done > 1 ? "s" : "");

	return 0;
}

int pvchange_single(struct cmd_context *cmd, struct physical_volume *pv)
{
	struct volume_group *vg = NULL;
	struct pv_list *pvl;

	const char *pv_name = dev_name(pv->dev);

	int allocatable =
	    !strcmp(arg_str_value(cmd, allocatable_ARG, "n"), "y");

	/* If in a VG, must change using volume group. */
	if (*pv->vg_name) {
		log_verbose("Finding volume group of physical volume \"%s\"",
			    pv_name);

		if (!lock_vol(cmd, pv->vg_name, LCK_VG_WRITE)) {
			log_error("Can't get lock for %s", pv->vg_name);
			return ECMD_FAILED;
		}

		if (!(vg = vg_read(cmd, pv->vg_name))) {
			unlock_vg(cmd, pv->vg_name);
			log_error("Unable to find volume group of \"%s\"",
				  pv_name);
			return 0;
		}

		if (vg->status & EXPORTED_VG) {
			unlock_vg(cmd, pv->vg_name);
			log_error("Volume group \"%s\" is exported", vg->name);
			return ECMD_FAILED;
		}

		if (!(vg->status & LVM_WRITE)) {
			unlock_vg(cmd, pv->vg_name);
			log_error("Volume group \"%s\" is read-only", vg->name);
			return ECMD_FAILED;
		}

		if (!(pvl = find_pv_in_vg(vg, pv_name))) {
			unlock_vg(cmd, pv->vg_name);
			log_error
			    ("Unable to find \"%s\" in volume group \"%s\"",
			     pv_name, vg->name);
			return 0;
		}
		pv = pvl->pv;
		if (!archive(vg))
			return 0;
	}

	/* change allocatability for a PV */
	if (allocatable && (pv->status & ALLOCATABLE_PV)) {
		log_error("Physical volume \"%s\" is already allocatable",
			  pv_name);
		if (*pv->vg_name)
			unlock_vg(cmd, pv->vg_name);
		return 0;
	}

	if (!allocatable && !(pv->status & ALLOCATABLE_PV)) {
		log_error("Physical volume \"%s\" is already unallocatable",
			  pv_name);
		if (*pv->vg_name)
			unlock_vg(cmd, pv->vg_name);
		return 0;
	}

	if (allocatable) {
		log_verbose("Setting physical volume \"%s\" allocatable",
			    pv_name);
		pv->status |= ALLOCATABLE_PV;
	} else {
		log_verbose("Setting physical volume \"%s\" NOT allocatable",
			    pv_name);
		pv->status &= ~ALLOCATABLE_PV;
	}

	log_verbose("Updating physical volume \"%s\"", pv_name);
	if (*pv->vg_name) {
		if (!vg_write(vg)) {
			unlock_vg(cmd, pv->vg_name);
			log_error("Failed to store physical volume \"%s\" in "
				  "volume group \"%s\"", pv_name, vg->name);
			return 0;
		}
		backup(vg);
		unlock_vg(cmd, pv->vg_name);
	} else {
		if (!(pv_write(cmd, pv))) {
			log_error("Failed to store physical volume \"%s\"",
				  pv_name);
			return 0;
		}
	}

	log_print("Physical volume \"%s\" changed", pv_name);

	return 1;
}
