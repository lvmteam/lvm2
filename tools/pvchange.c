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

void pvchange_single_volume(struct physical_volume *pv);

int pvchange(int argc, char **argv)
{
	int back_it_up = 0;
	int change_msg = 0;
	int done = 0;
	int doit = 0;
	int not_done = 0;
	int allocation;

	int opt = 0;
	int ret = 0;

	char *pv_name;
	char *vg_name;

	struct physical_volume *pv = NULL;
	struct volume_group *vg = NULL;
	struct device *pv_dev;
	struct list_head *pvh;
	struct pv_list *pvl, *pvs_list;

	allocation = !strcmp(arg_str_value(allocation_ARG, "n"), "y");

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

	if (arg_count(all_ARG)) {
		log_verbose("Scanning for physical volume names");
		if (!(pvs_list = ios->get_pvs(ios))) {
			return ECMD_FAILED;
		}

		list_for_each(pvh, &pvs_list->list) {
			pvl = list_entry(pvh, struct pv_list, list);
			pv = &pvl->pv;
			pvchange_single_volume(pv);
		}
	} else {
		for (; opt < argc; opt++) {
			pv_name = argv[opt];
			if (!(pv_dev = dev_cache_get(pv_name))) {
				log_error("Device %s not found", pv_name);
				continue;
			}
			if (!(pv = ios->pv_read(ios, pv_dev))) {
				log_error("Failed to read physical volume %s",
					  pv_name);
				continue;
			}
			pvchange_single_volume(pv);
		}
	}

	if (back_it_up)
		if ((ret = do_autobackup(vg_name, vg)))
			return ret;

	log_print("%d physical volume%s changed / %d physical volume%s "
		  "already o.k.", done, done != 1 ? "s" : "", not_done,
		  not_done != 1 ? "s" : "");

	return 0;
}

void pvchange_single_volume(struct physical_volume *pv)
{
	struct volume_group *vg;

	change_msg = 0;

	/* FIXME: Check these verbose messages appear in the library now */
	/* log_verbose("reading physical volume data %s from disk", pv_name); */

	/* FIXME: Ensure these are tested in the library */
	/* log_error("physical volume %s has an invalid version", pv_name); */
	/* log_error("physical volume %s has invalid identity", pv_name); */

	/* FIXME: Where do consistency checks fit? */
	/* log_verbose("checking physical volume %s consistency", pv_name); */

	/* FIXME: Does the VG really have to be active to proceed? */
	log_verbose("finding volume group of physical volume %s", pv_name);
	if (!(vg = ios->vg_read(ios, pv->vg_name))) {
		log_print("unable to find volume group of %s (VG not active?)",
			  pv->dev->name);
		doit = 0;
		return;
	}

	back_it_up = doit = 1;

	/* change allocatability for a PV */
	if (arg_count(allocation_ARG) > 0) {
		if (allocation && (pv->status & ALLOCATED_PV)) {
			log_error("physical volume %s is allocatable", pv_name);
			not_done++;
			return;
		} else
			change_msg = 1;

		if (!allocation && !(pv->status & ALLOCATED_PV)) {
			log_error("physical volume %s is unallocatable",
				  pv_name);
			not_done++;
			return;
		} else
			change_msg = 1;

		if (allocation) {
			log_verbose
			    ("setting physical volume %s allocatable", pv_name);
			pv->status |= ALLOCATED_PV;
		} else {
			log_verbose
			    ("setting physical volume %s NOT allocatable",
			     pv_name);
			pv->status &= ~ALLOCATED_PV;
		}
	}

	done++;

	if (doit == 1) {
		log_verbose("checking physical volume %s is activite",
			    pv->dev->name);
		if (!(pv->status & ACTIVE)) {
			log_verbose("Physical volume %s inactive", pv_name);
		}

		log_verbose("Updating physical volume %s", pv->dev->name);
		if (!(ios->pv_write(ios, pv))) {
			log_error
			    ("Failed to store physical volume %s",
			     pv->dev->name);
			/* Abort completely here? */
			return LVM_E_PV_WRITE;
		}

		log_print("physical volume %s %s changed", pv_name,
			  (change_msg) ? "" : "not ");
	}
}
