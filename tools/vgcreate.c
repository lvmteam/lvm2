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

/* FIXME Temporarily to get at MAX_PV etc.  Move these tests elsewhere */
/* FIXME Also PE_SIZE stuff */
#include "../lib/format1/disk-rep.h"

int vgcreate(int argc, char **argv)
{
	int opt;

	int max_lv = MAX_LV - 1;
	int max_pv = MAX_PV - 1;
	int min_pv_index = 0;
	ulong max_pv_size = 0;
	ulong min_pv_size = -1;

	/* long pe_size = LVM_DEFAULT_PE_SIZE; */

	char *dummy;
	char *vg_name;

	struct volume_group *vg;
	struct physical_volume *pv;

	struct list_head *pvh;
	struct pv_list *pvl;

	char *pv_name = NULL;

	if (arg_count(maxlogicalvolumes_ARG))
		max_lv = arg_int_value(maxlogicalvolumes_ARG, 0);

	if (arg_count(maxphysicalvolumes_ARG))
		max_pv = arg_int_value(maxphysicalvolumes_ARG, 0);

	if (arg_count(physicalextentsize_ARG)) {
/* FIXME - Move?
		pe_size = arg_int_value(physicalextentsize_ARG, 0);
		pe_size = ((unsigned long long) pe_size * 1024) / SECTOR_SIZE;
		if (vg_check_pe_size(pe_size) < 0) {
			log_error("Invalid physical extent size %s",
				  display_size(sectors_to_k(pe_size),
					       SIZE_SHORT));
			log_error("Must be power of 2 and between %s and %s",
				  display_size(sectors_to_k(LVM_MIN_PE_SIZE),
					       SIZE_SHORT),
				  display_size(sectors_to_k(LVM_MAX_PE_SIZE),
					       SIZE_SHORT));
			return EINVALID_CMD_LINE;
		}
*/
	}

	if (!argc) {
		log_error("Please provide volume group name and "
			  "physical volumes");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1) {
		log_error("Please enter physical volume name(s)");
		return EINVALID_CMD_LINE;
	}

	vg_name = argv[0];
	argv++;
	argc--;

	if ((vg = ios->vg_read(ios, vg_name))) {
		log_error("Volume group already exists: please use a "
			  "different name");
		return ECMD_FAILED;
	}

/***** FIXME: Can we be free of this restriction?
    log_verbose("Counting all existing volume groups");
    if (vg_count >= MAX_VG) {
	log_error("Maximum volume group count of %d reached", MAX_VG);
	return ECMD_FAILED;
    }
*****/

	if (!(vg = vg_create())) {
		return ECMD_FAILED;
	}

	/* check, if PVs are all defined and new */
	log_verbose("Ensuring all physical volumes specified are new");
	for (; opt < argc; opt++) {
		pv_name = argv[opt];
		if (!(pv = ios->pv_read(ios, pv_name))) {
			log_error("Physical volume %s not found", pv_name);
			return ECMD_FAILED;
		}

		/* FIXME Must PV be ACTIVE & ALLOCATABLE? */

		log_verbose("Checking physical volume %s", pv_name);
		log_verbose("Getting size of physical volume %s", pv_name);

		log_verbose("Physical volume %s is %s 512-byte sectors",
			    pv_name, pv->size);

		log_verbose("Checking physical volume %s is new", pv_name);
		if (*pv->vg_name) {
			log_error("%s already belongs to volume group %s",
				  pv_name, pv->vg_name);
		}

		log_verbose("Checking for identical physical volumes "
			    "on command line");
		if ((pvh = find_pv_in_vg(vg, pv_name))) {
			log_error("Physical volume %s listed multiple times",
				  pv_name);
			return ECMD_FAILED;
		}

		if (max_pv_size < pv->size)
			max_pv_size = pv->size;
		if (min_pv_size > pv->size) {
			min_pv_size = pv->size;
			min_pv_index = opt;
		}

		if (!(pvl = dbg_malloc(sizeof(struct pv_list)))) {
			log_error("pv_list allocation failed");
			return ECMD_FAILED;
		}

		pvl->pv = *pv;
		list_add(&pvl->list, &vg->pvs);
		vg->pv_count++;
	}

	log_verbose("%d physical volume(s) will be inserted into "
		    "volume group %s", vg->pv_count, vg->name);

	log_verbose("Maximum of %d physical volumes", max_pv);
	if (max_pv < 0 || max_pv <= vg->pv_count || max_pv > MAX_PV) {
		log_error("Invalid maximum physical volumes -p %d", max_pv);
		return EINVALID_CMD_LINE;
	}
	vg->max_pv = max_pv;

	log_verbose("Maximum of %d logical volumes", max_lv);
	if (max_lv < 0 || max_lv > MAX_LV) {
		log_error("Invalid maximum logical volumes -l %d", max_lv);
		return EINVALID_CMD_LINE;
	}
	vg->max_lv = max_lv;

/******** FIXME: Enforce these checks internally within vg_write?
    size = (LVM_PV_DISK_SIZE + LVM_VG_DISK_SIZE + max_pv * NAME_LEN + max_lv * sizeof(lv_t));
    if (size / SECTOR_SIZE > min_pv_size / 5) {
	log_error("More than 20%% [%d KB] of physical volume %s with %u KB would be used", size,
		  pvp[min_pv_index]->pv_name, pvp[min_pv_index]->pv_size / 2);
	return LVM_E_PV_TOO_SMALL;
    }
************/

/************ FIXME More work required here 
 ************ Check extent sizes compatible and set up pe's? 
	if (
	    (ret =
	     vg_setup_for_create(vg_name, &vg, pvp, pe_size, max_pv,
				 max_lv)) < 0) {
		if (ret == -LVM_EVG_SETUP_FOR_CREATE_PV_SIZE_MIN) {
			log_error
			    ("%d physical volume%s too small for physical extent size of %s",
			     count_sav, count_sav > 1 ? "s" : "", vg_name);
			log_error
			    ("Minimum physical volume at this physical extent size is %s",
			     (dummy =
			      display_size(sectors_to_k(vg.pe_size) *
					   LVM_PE_SIZE_PV_SIZE_REL,
					   SIZE_SHORT)));
			dbg_free(dummy);
		} else if (ret == -LVM_EVG_SETUP_FOR_CREATE_PV_SIZE_MAX) {
			log_error
			    ("%d physical volume%s too large for physical extent size of %s",
			     count_sav, count_sav > 1 ? "s" : "", vg_name);
			log_error
			    ("Maximum physical volume at this physical extent size is %s",
			     (dummy =
			      display_size(sectors_to_k(vg.pe_size) *
					   LVM_PE_T_MAX, SHORT)));
			dbg_free(dummy);
		}
		return ECMD_FAILED;
	}
************/

	if (arg_count(physicalextentsize_ARG) == 0) {
		log_print("Using default physical extent size %s",
			  (dummy = display_size(pe_size / 2, SIZE_SHORT)));
		dbg_free(dummy);
	}
	log_print("Maximum logical volume size is %s",
		  (dummy = display_size(LVM_LV_SIZE_MAX(&vg) / 2, SIZE_LONG)));
	dbg_free(dummy);

	vg_remove_dir_and_group_and_nodes(vg_name);

	vg->status |= ACTIVE;

	/* store vg on disk(s) */
	if (ios->vg_write(ios, vg)) {
		return ECMD_FAILED;
	}

/******* FIXME /dev/vg???
	log_verbose("Creating volume group directory %s%s", prefix, vg_name);
	if (vg_create_dir_and_group(&vg)) {
		return ECMD_FAILED;
	}
*********/

	/* FIXME Activate it */

/******* FIXME backups
	if ((ret = do_autobackup(vg_name, &vg)))
		return ret;
******/
	log_print("Volume group %s successfully created and activated",
		  vg_name);
	return 0;
}
