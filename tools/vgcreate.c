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

int vgcreate(int argc, char **argv)
{
	int count_sav = 0;
	int np = 0;
	int opt;
	int p = 0;
	int p1 = 0;
	int max_lv = MAX_LV - 1;
	int max_pv = MAX_PV - 1;
	int min_pv_index = 0;
	ulong max_pv_size = 0;
	ulong min_pv_size = -1;
	long pe_size = LVM_DEFAULT_PE_SIZE;
	int ret = 0;
	int size = 0;
	int v = 0;
	int vg_count = 0;

	char *dummy;
	char *vg_name;

	struct volume_group *vg;
	struct physical_volume *pv, **pvp = NULL;
	struct device pv_dev;

	char *pv_name = NULL;
	char **vg_name_ptr = NULL;

	struct io_space *ios;

	ios = active_ios();

	if (arg_count(maxlogicalvolumes_ARG))
		max_lv = arg_int_value(maxlogicalvolumes_ARG, 0);

	if (arg_count(maxphysicalvolumes_ARG))
		max_pv = arg_int_value(maxphysicalvolumes_ARG, 0);

	if (arg_count(physicalextentsize_ARG)) {
		pe_size = arg_int_value(physicalextentsize_ARG, 0);
		pe_size = ((unsigned long long) pe_size * 1024) / SECTOR_SIZE;
		if (vg_check_pe_size(pe_size) < 0) {
			log_error("invalid physical extent size %s",
				  display_size(sectors_to_k(pe_size),
					       SIZE_SHORT));
			log_error("must be power of 2 and between %s and %s",
				  display_size(sectors_to_k(LVM_MIN_PE_SIZE),
					       SIZE_SHORT),
				  display_size(sectors_to_k(LVM_MAX_PE_SIZE),
					       SIZE_SHORT));
			return EINVALID_CMD_LINE;
		}
	}

	if (argc == 0) {
		log_error
		    ("please enter a volume group name and physical volumes");
		return EINVALID_CMD_LINE;
	}
	vg_name = argv[0];

	if (argc == 1) {
		log_error("please enter physical volume name(s)");
		return EINVALID_CMD_LINE;
	}

	if ((vg = ios->vg_read(ios, vg_name))) {
		log_error
		    ("Volume group already exists: please use a different name");
		return ECMD_FAILED;
	}

/***** FIXME: confirm we're now free of this restriction

    log_verbose("counting all existing volume groups");
    vg_name_ptr = lvm_tab_vg_check_exist_all_vg();
    vg_count = 0;
    if (vg_name_ptr != NULL)
	for (v = 0; vg_name_ptr[v] != NULL && vg_count < MAX_VG; v++)
	    vg_count++;
    if (vg_count >= MAX_VG) {
	log_error("maximum volume group count of %d reached", MAX_VG);
	return LVM_E_MAX_VG;
    }
*****/

	if (!(vg = vg_create())) {
		return ECMD_FAILED;
	}

	/* read all PVs */

	/* check, if PVs are all defined and new */
	log_verbose("Checking all physical volumes specified are new");
	count_sav = argc - 1;
	np = 0;
	for (opt = 1; opt < argc; opt++) {
		pv_name = argv[opt];

		if (!(pv_dev = dev_cache_get(pv_name))) {
			log_error("Device %s not found", pv_name);
			return ECMD_FAILED;
		}

		if (!(pv = ios->pv_read(ios, pv_dev))) {
			log_error("Physical volume %s not found", pv_name);
			return ECMD_FAILED;
		}

		log_verbose("checking physical volume %s", pv_name);
		log_verbose("getting size of physical volume %s", pv_name);

		/* FIXME size should already be filled in pv structure?! */
		if ((size = dev_get_size(pv_dev)) < 0) {
			log_error("Unable to get size of %s", pv_name);
			return ECMD_FAILED;
		}

		log_verbose("physical volume %s  is %d 512-byte sectors",
			    pv_name, size);

		log_verbose("checking physical volume %s is new", pv_name);
		if (pv->vg_name[0]) {
			log_error("%s already belongs to volume group %s",
				  pv_name pv->vg_name);
		}

		log_verbose("checking for identical physical volumes "
			    "on command line");
		for (p1 = 0; pvp != NULL && pvp[p1] != NULL; p1++) {
			if (!strcmp(pv_name, pvp[p1]->dev->name)) {
				log_error
				    ("physical volume %s occurs multiple times",
				     pv_name);
				return ECMD_FAILED;
			}
		}

		if ((pvp = dbg_realloc(pvp, (np + 2) * sizeof (pv *))) == NULL) {
			log_error("realloc error in file \"%s\" [line %d]",
				  __FILE__, __LINE__);
			return ECMD_FAILED;
		}

		pvp[np] = pv;
		if (max_pv_size < pvp[np]->size)
			max_pv_size = pvp[np]->size;
		if (min_pv_size > pvp[np]->size) {
			min_pv_size = pvp[np]->size;
			min_pv_index = np;
		}
		np++;
		pvp[np] = NULL;
	}

	if (np == 0) {
		log_error("no valid physical volumes in command line");
		return ECMD_FAILED;
	}

	if (np != count_sav) {
		log_error("some invalid physical volumes in command line");
		return ECMD_FAILED;	/* Impossible to reach here? */
	}

	log_verbose("%d physical volume%s will be inserted into "
		    "volume group %s", np, np > 1 ? "s" : "", vg_name);

	vg->pv = pvp;

	/* load volume group */
	/* checking command line arguments */
	log_verbose("checking command line arguments");

	log_verbose("maximum of %d physical volumes", max_pv);
	if (max_pv < 0 || max_pv <= np || max_pv > MAX_PV) {
		log_error("invalid maximum physical volumes -p %d", max_pv);
		return EINVALID_CMD_LINE;
	}
	vg->max_pv = max_pv;

	log_verbose("maximum of %d logical volumes", max_lv);
	if (max_lv < 0 || max_lv > MAX_LV) {
		log_error("invalid maximum logical volumes -l %d", max_lv);
		return EINVALID_CMD_LINE;
	}
	vg->max_lv = max_lv;

/******** FIXME: Enforce these checks internally within vg_write?
    size = (LVM_PV_DISK_SIZE + LVM_VG_DISK_SIZE + max_pv * NAME_LEN + max_lv * sizeof(lv_t));
    if (size / SECTOR_SIZE > min_pv_size / 5) {
	log_error("more than 20%% [%d KB] of physical volume %s with %u KB would be used", size,
		  pvp[min_pv_index]->pv_name, pvp[min_pv_index]->pv_size / 2);
	return LVM_E_PV_TOO_SMALL;
    }
************/

	/* FIXME More work required here */
	/* Check extent sizes compatible and set up pe's? */
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

	if (arg_count(physicalextentsize_ARG) == 0) {
		log_print("Using default physical extent size %s",
			  (dummy = display_size(pe_size / 2, SIZE_SHORT)));
		dbg_free(dummy);
	}
	log_print("Maximum logical volume size is %s",
		  (dummy = display_size(LVM_LV_SIZE_MAX(&vg) / 2, SIZE_LONG)));
	dbg_free(dummy);

	vg_remove_dir_and_group_and_nodes(vg_name);

	/* FIXME Set active flag */

	/* store vg on disk(s) */
	if (ios->vg_write(ios, vg)) {
		return ECMD_FAILED;
	}

	log_verbose("creating volume group directory %s%s", prefix, vg_name);
	if (vg_create_dir_and_group(&vg)) {
		return ECMD_FAILED;
	}

	/* FIXME Activate it */

	if ((ret = do_autobackup(vg_name, &vg)))
		return ret;
	log_print("Volume group %s successfully created and activated",
		  vg_name);
	return 0;
}
