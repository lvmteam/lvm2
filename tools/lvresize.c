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

int lvresize(int argc, char **argv)
{
	struct volume_group *vg;
	struct logical_volume *lv;
	uint32_t extents = 0;
	uint32_t size = 0;
	uint32_t stripes = 0, stripesize = 0;
	uint32_t seg_stripes = 0, seg_stripesize = 0, seg_size = 0;
	uint32_t size_rest;
	sign_t sign = SIGN_NONE;
	char *lv_name, *vg_name;
	char *st;
	char *dummy;
	const char *cmd_name;
	struct list *lvh, *pvh, *pvl, *segh;
	int opt = 0;

	enum {
		LV_ANY = 0,
		LV_REDUCE = 1,
		LV_EXTEND = 2
	} resize = LV_ANY;

	cmd_name = command_name();
	if (!strcmp(cmd_name, "lvreduce"))
		resize = LV_REDUCE;
	if (!strcmp(cmd_name, "lvextend"))
		resize = LV_EXTEND;

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
		sign = arg_sign_value(size_ARG, SIGN_NONE);
	}

	if (resize == LV_EXTEND && sign == SIGN_MINUS) {
		log_error("Negative argument not permitted - use lvreduce");
		return EINVALID_CMD_LINE;
	}

	if (resize == LV_REDUCE && sign == SIGN_PLUS) {
		log_error("Positive sign not permitted - use lvextend");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(stripes_ARG)) {
		log_print("Stripes not yet implemented in LVM2. Ignoring.");
		stripes = arg_int_value(stripes_ARG, 1);
	}

	if (arg_count(stripesize_ARG))
		stripesize = 2 * arg_int_value(stripesize_ARG, 0);

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

/******* Remove requirement 
	if (!(vg->status & ACTIVE)) {
		log_error("Volume group %s must be active before changing a "
			  "logical volume", vg_name);
		return ECMD_FAILED;
	}
********/

	/* does LV exist? */
	if (!(lvh = find_lv_in_vg(vg, lv_name))) {
		log_error("Logical volume %s not found in volume group %s",
			  lv_name, vg_name);
		return ECMD_FAILED;
	}

	lv = &list_item(lvh, struct lv_list)->lv;

/******* Remove requirement
	if (!(lv->status & ACTIVE)) {
		log_error("Logical volume %s must be active before change",
			  lv_name);
		return ECMD_FAILED;
	}
********/

	if (size) {
		/* No of 512-byte sectors */
		extents = size * 2;

		if (extents % vg->extent_size) {
			char *s1;

			if (sign == SIGN_MINUS)
				extents -= extents % vg->extent_size;
			else
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

	if (sign == SIGN_MINUS) {
		if (extents >= lv->le_count) {
			log_error("Unable to reduce %s below 1 extent",
				  lv_name);
			return EINVALID_CMD_LINE;
		}

		extents = lv->le_count - extents;
	}

	if (!extents) {
		log_error("New size of 0 not permitted");
		return EINVALID_CMD_LINE;
	}

	if (extents == lv->le_count) {
		log_error("New size (%d extents) matches existing size "
			  "(%d extents)", extents, lv->le_count);
		return EINVALID_CMD_LINE;
	}

	/* If extending, find stripes, stripesize & size of last segment */
	if (extents > lv->le_count && 
	    !(stripes == 1 || (stripes > 1 && stripesize))) {
		list_iterate(segh, &lv->segments) {
			struct stripe_segment *seg;
			uint32_t sz, str;

 			seg = list_item(segh, struct stripe_segment);
			sz = seg->stripe_size;
			str = seg->stripes;

			if ((seg_stripesize && seg_stripesize != sz
			     && !stripesize) ||
			    (seg_stripes && seg_stripes != str && !stripes)) {
				log_error("Please specify number of "
					  "stripes (-i) and stripesize (-I)");
				return EINVALID_CMD_LINE;
			}

			seg_stripesize = sz;
			seg_stripes = str;
		}

		if (!stripesize && stripes > 1)
			stripesize = seg_stripesize;

		if (!stripes)
			stripes = seg_stripes;

		seg_size = extents - lv->le_count;
	}

	/* If reducing, find stripes, stripesize & size of last segment */
	if (extents < lv->le_count) {
		uint32_t extents_used = 0;

		if (stripes || stripesize)
			log_error("Ignoring stripes and stripesize arguments "
				  "when reducing");

		list_iterate(segh, &lv->segments) {
			struct stripe_segment *seg;
			uint32_t seg_extents;

			seg = list_item(segh, struct stripe_segment);
			seg_extents = seg->len;

			seg_stripesize = seg->stripe_size;
			seg_stripes = seg->stripes;

			if (extents <= extents_used + seg_extents)
				break;

			extents_used += seg_extents;
		}

		seg_size = extents - extents_used;
		stripesize = seg_stripesize;
		stripes = seg_stripes;
	}

	if ((size_rest = seg_size % stripes)) {
		log_print("Rounding size (%d extents) down to stripe boundary "
			  "size of last segment (%d extents)", extents,
			  extents - size_rest);
		extents = extents - size_rest;
	}

	if (extents == lv->le_count) {
		log_error("New size (%d extents) matches existing size "
			  "(%d extents)", extents, lv->le_count);
		return EINVALID_CMD_LINE;
	}

	if (extents < lv->le_count) {
		if (resize == LV_EXTEND) {
			log_error("New size given (%d extents) not larger "
				  "than existing size (%d extents)",
				  extents, lv->le_count);
			return EINVALID_CMD_LINE;
		} else
			resize = LV_REDUCE;
	}

	if (extents > lv->le_count) {
		if (resize == LV_REDUCE) {
			log_error("New size given (%d extents) not less than "
				  "existing size (%d extents)", extents,
				  lv->le_count);
			return EINVALID_CMD_LINE;
		} else
			resize = LV_EXTEND;
	}

	if (resize == LV_REDUCE) {
		if (argc)
			log_print("Ignoring PVs on command line when reducing");

		if (lv_active(lv)) {
			dummy =
			    display_size(extents * vg->extent_size / 2,
					 SIZE_SHORT);
			log_print("WARNING: Reducing active%s logical volume "
				  "to %s", lv_open_count(lv) ? " and open" : "",
				  dummy);

			log_print("THIS MAY DESTROY YOUR DATA "
				  "(filesystem etc.)");
			dbg_free(dummy);
		}

		if (!arg_count(force_ARG)) {
			if (yes_no_prompt("Do you really want to reduce %s?"
					  " [y/n]: ", lv_name) == 'n') {
				log_print("Logical volume %s NOT reduced",
					  lv_name);
				return ECMD_FAILED;
			}
		}

		if (!lv_reduce(lv, lv->le_count - extents))
			return ECMD_FAILED;
	}

	if (resize == LV_EXTEND && argc) {
		/* Build up list of PVs */
		if (!(pvh = pool_alloc(fid->cmd->mem, sizeof(struct list)))) {
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
	}

	if (resize == LV_EXTEND) {
		if (!argc) {
			/* Use full list from VG */
			pvh = &vg->pvs;
		}
		dummy = display_size(extents * vg->extent_size / 2, SIZE_SHORT);
		log_print("Extending logical volume %s to %s", lv_name, dummy);
		dbg_free(dummy);

		if (!lv_extend(lv, stripes, stripesize, extents - lv->le_count,
			       pvh))
			return ECMD_FAILED;
	}

/********* FIXME Suspend lv  ***********/

	/* store vg on disk(s) */
	if (!fid->ops->vg_write(fid, vg))
		return ECMD_FAILED;

        backup(vg);

	/* FIXME Ensure it always displays errors? */
	if (!lv_reactivate(lv))
		return ECMD_FAILED;

/********* FIXME Resume *********/

	log_print("Logical volume %s successfully resized", lv_name);

	return 0;
}
