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

int pvchange_single(struct physical_volume *pv);

int pvchange(int argc, char **argv)
{
	int opt = 0;
	int done = 0;
	int total = 0;

	struct physical_volume *pv;
	char *pv_name;

	struct list *pvh, *pvs;

	if (arg_count(allocation_ARG) == 0) {
		log_error("Please give the x option");
		return EINVALID_CMD_LINE;
	}

	if (!(arg_count(all_ARG)) && !argc) {
		log_error("Please give a physical volume path");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(all_ARG) && argc) {
		log_error("Option a and PhysicalVolumePath are exclusive");
		return EINVALID_CMD_LINE;
	}

	if (argc) {
		log_verbose("Using physical volume(s) on command line");
		for (; opt < argc; opt++) {
			pv_name = argv[opt];
			if (!(pv = fid->ops->pv_read(fid, pv_name))) {
				log_error("Failed to read physical volume %s",
					  pv_name);
				continue;
			}
			total++;
			done += pvchange_single(pv);
		}
	} else {
		log_verbose("Scanning for physical volume names");
		if (!(pvs = fid->ops->get_pvs(fid))) {
			return ECMD_FAILED;
		}

		list_iterate(pvh, pvs) {
			total++;
			done += pvchange_single(
				&list_item(pvh, struct pv_list)->pv);
		}
	}

/******* FIXME Backup
	if ((ret = do_autobackup(vg_name, vg)))
		return ret;
*********/

	log_print("%d physical volume(s) changed / %d physical volume(s) "
		  "not changed", done, total - done);

	return 0;
}

int pvchange_single(struct physical_volume *pv)
{
	struct volume_group *vg = NULL;
	struct list *pvh;

	const char *pv_name = dev_name(pv->dev);

	int allocation = !strcmp(arg_str_value(allocation_ARG, "n"), "y");

	/* If in a VG, must change using volume group.  Pointless. */
	/* FIXME: Provide a direct pv_write_pv that *only* touches PV structs*/
	if (*pv->vg_name) {
		log_verbose("Finding volume group of physical volume %s", 
			    pv_name);
		if (!(vg = fid->ops->vg_read(fid, pv->vg_name))) {
			log_error("Unable to find volume group of %s", pv_name);
			return 0;
		}
		if (!(pvh = find_pv_in_vg(vg, pv_name))) {
			log_error("Unable to find %s in volume group %s",
				pv_name, vg->name);
			return 0;
		}
		pv = &list_item(pvh, struct pv_list)->pv;
	}

	/* change allocatability for a PV */
	if (allocation && (pv->status & ALLOCATED_PV)) {
		log_error("Physical volume %s is already allocatable", pv_name);
		return 0;
	}

	if (!allocation && !(pv->status & ALLOCATED_PV)) {
		log_error("Physical volume %s is already unallocatable",
			  pv_name);
		return 0;
	}

	if (allocation) {
		log_verbose("Setting physical volume %s allocatable", pv_name);
		pv->status |= ALLOCATED_PV;
	} else {
		log_verbose("Setting physical volume %s NOT allocatable",
			    pv_name);
		pv->status &= ~ALLOCATED_PV;
	}

	if (!(pv->status & ACTIVE)) {
		log_verbose("Physical volume %s inactive", pv_name);
	}

	log_verbose("Updating physical volume %s", pv_name);
	if (*pv->vg_name) {
		if (!(fid->ops->vg_write(fid,vg))) {
			log_error("Failed to store physical volume %s in "
				  "volume group %s", pv_name, vg->name);
			return 0;
		}
	} else {
		if (!(fid->ops->pv_write(fid, pv))) {
			log_error("Failed to store physical volume %s", 
				  pv_name);
			return 0;
		}
	}

	log_print("Physical volume %s changed", pv_name);

	return 1;
}
