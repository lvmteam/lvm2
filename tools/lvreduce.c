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

int lvreduce(int argc, char **argv)
{
	struct volume_group *vg;
	struct logical_volume *lv;
	struct list *lvh;
	int32_t extents = 0;
	int32_t size = 0;
	char *lv_name, *vg_name;
	char *st;

	if (arg_count(extents_ARG) + arg_count(size_ARG) != 1) {
		log_error("Please specify either size or extents (not both)");
		return EINVALID_CMD_LINE;
	}

	/* FIXME signed use throughout halves the maximum... */
	if (arg_count(extents_ARG))
		extents = arg_int_value(extents_ARG, 0);

	if (arg_count(size_ARG))
		size = arg_int_value(size_ARG, 0);

	if (!argc) {
		log_error("Please provide the logical volume name");
		return EINVALID_CMD_LINE;
	}

	lv_name = argv[0];
	argv++;
	argc--;

	if (!(vg_name = extract_vgname(ios, lv_name))) {
		log_error("Please provide a volume group name");
		return EINVALID_CMD_LINE;
	}

	if ((st = strrchr(lv_name, '/')))
		lv_name = st + 1;

	/* does VG exist? */
	log_verbose("Finding volume group %s", vg_name);
	if (!(vg = ios->vg_read(ios, vg_name))) {
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
		log_error("Logical volume %s must be active before reduction",
			  lv_name);
		return ECMD_FAILED;
	}

	if (size) {
		/* No of 512-byte sectors */
		extents = size * 2;

		if (extents % vg->extent_size) {
			char *s1;

			extents += (signed) vg->extent_size -
			    (signed) (extents % vg->extent_size);
			log_print("Rounding up size to full physical extent %s",
				  (s1 =
				   display_size(abs(extents) / 2, SIZE_SHORT)));
			dbg_free(s1);
		}

		extents /= (signed) vg->extent_size;
	}

	if (!extents) {
		log_error("New size of 0 not permitted");
		return EINVALID_CMD_LINE;
	}

	if (extents < 0) {
		if (1 - extents > lv->le_count) {
			log_error("Unable to reduce %s below 1 extent",
				  lv_name);
			return EINVALID_CMD_LINE;
		}

		lv->le_count += extents;
	} else {
		if (extents >= lv->le_count) {
			log_error("New size given (%d extents) not less than "
				  "existing size (%d extents)", extents,
				  lv->le_count);
			return EINVALID_CMD_LINE;
		}
		lv->le_count = extents;
	}

/************ FIXME Stripes
		size_rest = new_size % (vg->lv[l]->lv_stripes * vg->pe_size);
		if (size_rest != 0) {
			log_print
			    ("rounding size %ld KB to stripe boundary size ",
			     new_size / 2);
			new_size = new_size - size_rest;
			printf("%ld KB\n", new_size / 2);
		}
***********************/

	if (lv->status & ACTIVE || lv_active(lv) > 0) {
		char *dummy;
		log_print("WARNING: Reducing active%s logical volume to %s",
			  (lv_open_count(lv) > 0) ? " and open" : "",
			  (dummy =
			   display_size(lv->le_count * vg->extent_size / 2,
					SIZE_SHORT)));
		log_print("THIS MAY DESTROY YOUR DATA (filesystem etc.)");
		dbg_free(dummy);
	}

	if (!arg_count(force_ARG)) {
		if (yes_no_prompt
		    ("Do you really want to reduce %s? [y/n]: ", lv_name)
		    == 'n') {
			log_print("Logical volume %s NOT reduced", lv_name);
			return ECMD_FAILED;
		}
	}

/********* FIXME Suspend lv  ***********/

	/* store vg on disk(s) */
	if (!ios->vg_write(ios, vg))
		return ECMD_FAILED;

	/* FIXME Ensure it always displays errors? */
	if (!lv_reactivate(lv))
		return ECMD_FAILED;

/********* FIXME Resume *********/

/********* FIXME Backup 
	if ((ret = do_autobackup(vg_name, vg)))
		return ret;
************/

	log_print("Logical volume %s reduced", lv_name);

	return 0;
}

