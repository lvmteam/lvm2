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

int lvextend(int argc, char **argv)
{
	struct volume_group *vg;
	struct logical_volume *lv;
	uint32_t extents = 0;
	uint32_t size = 0;
	sign_t sign = SIGN_NONE;
	char *lv_name, *vg_name;
	char *st;
	char *dummy;
	struct list *lvh, *pvh, *pvl;
	int opt = 0;

	if (arg_count(extents_ARG) + arg_count(size_ARG) != 1) {
		log_error("Please specify either size or extents (not both)");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(extents_ARG)) {
		extents = arg_int_value(extents_ARG, 0);
		sign = arg_sign_value(extents_ARG, SIGN_NONE);
	}

	if (arg_count(size_ARG)) {
		size = arg_int_value(size_ARG, 0);
		sign = arg_sign_value(extents_ARG, SIGN_NONE);
	}

	if (sign == SIGN_MINUS) {
		log_error("Negative argument not permitted - use lvreduce");
		return EINVALID_CMD_LINE;
	}

	if (!argc) {
		log_error("Please provide the logical volume name");
		return EINVALID_CMD_LINE;
	}

	lv_name = argv[0];
	argv++;
	argc--;

	if (!(vg_name = extract_vgname(fid, lv_name))) {
		log_error("Please provide a volume group name");
		return EINVALID_CMD_LINE;
	}

	if ((st = strrchr(lv_name, '/')))
		lv_name = st + 1;

	/* does VG exist? */
	log_verbose("Finding volume group %s", vg_name);
	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Volume group %s doesn't exist", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg->status & ACTIVE)) {
		log_error("Volume group %s must be active before changing a "
			  "logical volume", vg_name);
		return ECMD_FAILED;
	}

	/* does LV exist? */
	if (!(lvh = find_lv_in_vg(vg, lv_name))) {
		log_error("Logical volume %s not found in volume group %s",
			  lv_name, vg_name);
		return ECMD_FAILED;
	}

	lv = &list_item(lvh, struct lv_list)->lv;

	if (!(lv->status & ACTIVE)) {
		log_error("Logical volume %s must be active before change",
			  lv_name);
		return ECMD_FAILED;
	}

	if (argc) {
		/* Build up list of PVs */
		if (!(pvh = pool_alloc(fid->cmd->mem, sizeof (struct list)))) {
			log_error("pvh list allocation failed");
			return ECMD_FAILED;
		}
		list_init(pvh);
		for (; opt < argc; opt++) {
			if (!(pvl = find_pv_in_vg(vg, argv[opt]))) {
				log_error("Physical Volume %s not found in "
					  "Volume Group %s", argv[opt],
					  vg->name);
				return EINVALID_CMD_LINE;
			}
			if (list_item(pvl, struct pv_list)->pv.pe_count ==
			    list_item(pvl, struct pv_list)->pv.pe_allocated) {
				log_error("No free extents on physical volume"
					  " %s", argv[opt]);
				continue;
				/* FIXME Buy check not empty at end! */
			}
			list_add(pvh, pvl);
		}
	} else {
		/* Use full list from VG */
		pvh = &vg->pvs;
	}

	if (size) {
		/* No of 512-byte sectors */
		extents = size * 2;

		if (extents % vg->extent_size) {
			char *s1;

			extents += vg->extent_size -
			    (extents % vg->extent_size);
			log_print("Rounding up size to full physical extent %s",
				  (s1 = display_size(extents / 2, SIZE_SHORT)));
			dbg_free(s1);
		}

		extents /= vg->extent_size;
	}

	if (sign == SIGN_PLUS)
		extents += lv->le_count;

	if (extents <= lv->le_count) {
		log_error("New size given (%d extents) not larger than "
			  "existing size (%d extents)", extents, lv->le_count);
		return EINVALID_CMD_LINE;
	}

	if (!extents) {
		log_error("New size of 0 not permitted");
		return EINVALID_CMD_LINE;
	}

	log_print("Extending logical volume %s to %s", lv_name,
		  (dummy =
		   display_size(extents * vg->extent_size / 2, SIZE_SHORT)));
	dbg_free(dummy);

	lv_extend(lv, extents - lv->le_count, pvh);
	/* where parm is always *increase* not actual */

/********* FIXME Suspend lv  ***********/

	/* store vg on disk(s) */
	if (!fid->ops->vg_write(fid, vg))
		return ECMD_FAILED;

	/* FIXME Ensure it always displays errors? */
	if (!lv_reactivate(lv))
		return ECMD_FAILED;

/********* FIXME Resume *********/

/********* FIXME Backup 
        if ((ret = do_autobackup(vg_name, vg)))
                return ret;
************/

	log_print("Logical volume %s successfully extended", lv_name);

	return 0;
}
