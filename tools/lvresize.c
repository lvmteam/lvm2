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

int lvresize(struct cmd_context *cmd, int argc, char **argv)
{
	struct volume_group *vg;
	struct logical_volume *lv;
	struct dm_info info;
	uint32_t extents = 0;
	uint32_t size = 0;
	uint32_t stripes = 0, ssize = 0, stripesize_extents = 0;
	uint32_t seg_stripes = 0, seg_stripesize = 0, seg_size = 0;
	uint32_t extents_used = 0;
	uint32_t size_rest;
	sign_t sign = SIGN_NONE;
	char *lv_name;
	const char *vg_name;
	char *st;
	const char *cmd_name;
	struct list *pvh, *segh;
	struct lv_list *lvl;
	int opt = 0;
	int consistent = 1;

	enum {
		LV_ANY = 0,
		LV_REDUCE = 1,
		LV_EXTEND = 2
	} resize = LV_ANY;

	cmd_name = command_name(cmd);
	if (!strcmp(cmd_name, "lvreduce"))
		resize = LV_REDUCE;
	if (!strcmp(cmd_name, "lvextend"))
		resize = LV_EXTEND;

	if (arg_count(cmd, extents_ARG) + arg_count(cmd, size_ARG) != 1) {
		log_error("Please specify either size or extents (not both)");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, extents_ARG)) {
		extents = arg_uint_value(cmd, extents_ARG, 0);
		sign = arg_sign_value(cmd, extents_ARG, SIGN_NONE);
	}

	if (arg_count(cmd, size_ARG)) {
		size = arg_uint_value(cmd, size_ARG, 0);
		sign = arg_sign_value(cmd, size_ARG, SIGN_NONE);
	}

	if (resize == LV_EXTEND && sign == SIGN_MINUS) {
		log_error("Negative argument not permitted - use lvreduce");
		return EINVALID_CMD_LINE;
	}

	if (resize == LV_REDUCE && sign == SIGN_PLUS) {
		log_error("Positive sign not permitted - use lvextend");
		return EINVALID_CMD_LINE;
	}

	if (!argc) {
		log_error("Please provide the logical volume name");
		return EINVALID_CMD_LINE;
	}

	lv_name = argv[0];
	argv++;
	argc--;

	if (!(vg_name = extract_vgname(cmd, lv_name))) {
		log_error("Please provide a volume group name");
		return EINVALID_CMD_LINE;
	}

	if ((st = strrchr(lv_name, '/')))
		lv_name = st + 1;

	/* does VG exist? */
	log_verbose("Finding volume group %s", vg_name);
	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg = vg_read(cmd, vg_name, &consistent))) {
		log_error("Volume group %s doesn't exist", vg_name);
		goto error;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group %s is exported", vg->name);
		goto error;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group %s is read-only", vg_name);
		goto error;
	}

	/* does LV exist? */
	if (!(lvl = find_lv_in_vg(vg, lv_name))) {
		log_error("Logical volume %s not found in volume group %s",
			  lv_name, vg_name);
		goto error;
	}

	if (arg_count(cmd, stripes_ARG)) {
		if (vg->fid->fmt->features & FMT_SEGMENTS)
			stripes = arg_uint_value(cmd, stripes_ARG, 1);
		else
			log_print("Varied striping not supported. Ignoring.");
	}

	if (arg_count(cmd, stripesize_ARG)) {
		if (vg->fid->fmt->features & FMT_SEGMENTS)
			ssize = 2 * arg_uint_value(cmd, stripesize_ARG, 0);
		else
			log_print("Varied stripesize not supported. Ignoring.");
	}

	lv = lvl->lv;

	if (size) {
		/* No of 512-byte sectors */
		extents = size * 2;

		if (extents % vg->extent_size) {
			if (sign == SIGN_MINUS)
				extents -= extents % vg->extent_size;
			else
				extents += vg->extent_size -
				    (extents % vg->extent_size);

			log_print("Rounding up size to full physical extent %s",
				  display_size(cmd, (uint64_t) extents / 2,
					       SIZE_SHORT));
		}

		extents /= vg->extent_size;
	}

	if (sign == SIGN_PLUS)
		extents += lv->le_count;

	if (sign == SIGN_MINUS) {
		if (extents >= lv->le_count) {
			log_error("Unable to reduce %s below 1 extent",
				  lv_name);
			goto error_cmdline;
		}

		extents = lv->le_count - extents;
	}

	if (!extents) {
		log_error("New size of 0 not permitted");
		goto error_cmdline;
	}

	if (extents == lv->le_count) {
		log_error("New size (%d extents) matches existing size "
			  "(%d extents)", extents, lv->le_count);
		goto error_cmdline;
	}

	seg_size = extents - lv->le_count;

	/* If extending, find stripes, stripesize & size of last segment */
	if (extents > lv->le_count && !(stripes == 1 || (stripes > 1 && ssize))) {
		list_iterate(segh, &lv->segments) {
			struct lv_segment *seg;
			uint32_t sz, str;

			seg = list_item(segh, struct lv_segment);
			sz = seg->stripe_size;
			str = seg->stripes;

			if ((seg_stripesize && seg_stripesize != sz
			     && !ssize) ||
			    (seg_stripes && seg_stripes != str && !stripes)) {
				log_error("Please specify number of "
					  "stripes (-i) and stripesize (-I)");
				goto error_cmdline;
			}

			seg_stripesize = sz;
			seg_stripes = str;
		}

		if (!stripes)
			stripes = seg_stripes;

		if (!ssize && stripes > 1) {
			if (seg_stripesize) {
				log_print("Using stripesize of last segment "
					  "%dKB", seg_stripesize / 2);
				ssize = seg_stripesize;
			} else {
				ssize = find_config_int(cmd->cf->root,
							"metadata/stripesize",
							'/',
							DEFAULT_STRIPESIZE) * 2;
				log_print("Using default stripesize %dKB",
					  ssize / 2);
			}
		}
	}

	/* If reducing, find stripes, stripesize & size of last segment */
	if (extents < lv->le_count) {
		extents_used = 0;

		if (stripes || ssize)
			log_error("Ignoring stripes and stripesize arguments "
				  "when reducing");

		list_iterate(segh, &lv->segments) {
			struct lv_segment *seg;
			uint32_t seg_extents;

			seg = list_item(segh, struct lv_segment);
			seg_extents = seg->len;

			seg_stripesize = seg->stripe_size;
			seg_stripes = seg->stripes;

			if (extents <= extents_used + seg_extents)
				break;

			extents_used += seg_extents;
		}

		seg_size = extents - extents_used;
		ssize = seg_stripesize;
		stripes = seg_stripes;
	}

	if (stripes > 1 && !ssize) {
		log_error("Stripesize for striped segment should not be 0!");
		goto error_cmdline;
	}

	if ((stripes > 1)) {
		if (!(stripesize_extents = ssize / vg->extent_size))
			stripesize_extents = 1;

		if ((size_rest = seg_size % (stripes * stripesize_extents))) {
			log_print("Rounding size (%d extents) down to stripe "
				  "boundary size for segment (%d extents)",
				  extents, extents - size_rest);
			extents = extents - size_rest;
		}
	}

	if (extents == lv->le_count) {
		log_error("New size (%d extents) matches existing size "
			  "(%d extents)", extents, lv->le_count);
		goto error_cmdline;
	}

	if (extents < lv->le_count) {
		if (resize == LV_EXTEND) {
			log_error("New size given (%d extents) not larger "
				  "than existing size (%d extents)",
				  extents, lv->le_count);
			goto error_cmdline;
		} else
			resize = LV_REDUCE;
	}

	if (extents > lv->le_count) {
		if (resize == LV_REDUCE) {
			log_error("New size given (%d extents) not less than "
				  "existing size (%d extents)", extents,
				  lv->le_count);
			goto error_cmdline;
		} else
			resize = LV_EXTEND;
	}

	if (resize == LV_REDUCE) {
		if (argc)
			log_print("Ignoring PVs on command line when reducing");

		memset(&info, 0, sizeof(info));

		if (!lv_info(lv, &info) && driver_version(NULL, 0)) {
			log_error("lv_info failed: aborting");
			goto error;
		}

		if (info.exists) {
			log_print("WARNING: Reducing active%s logical volume "
				  "to %s", info.open_count ? " and open" : "",
				  display_size(cmd, (uint64_t)
					       extents * (vg->extent_size / 2),
					       SIZE_SHORT));

			log_print("THIS MAY DESTROY YOUR DATA "
				  "(filesystem etc.)");
		}

		if (!arg_count(cmd, force_ARG)) {
			if (yes_no_prompt("Do you really want to reduce %s?"
					  " [y/n]: ", lv_name) == 'n') {
				log_print("Logical volume %s NOT reduced",
					  lv_name);
				goto error;
			}
		}

		if (!archive(vg))
			goto error;

		if (!lv_reduce(vg->fid, lv, lv->le_count - extents))
			goto error;
	}

	if (resize == LV_EXTEND) {
		if (!(pvh = argc ? create_pv_list(cmd->mem, vg, argc - opt,
						  argv + opt) : &vg->pvs)) {
			stack;
			goto error;
		}

		if (!archive(vg))
			goto error;

		log_print("Extending logical volume %s to %s", lv_name,
			  display_size(cmd, (uint64_t)
				       extents * (vg->extent_size / 2),
				       SIZE_SHORT));

		if (!lv_extend(vg->fid, lv, stripes, ssize,
			       extents - lv->le_count, pvh))
			goto error;
	}

	if (!lock_vol(cmd, lv->lvid.s, LCK_LV_SUSPEND | LCK_HOLD)) {
		log_error("Can't get lock for %s", lv_name);
		goto error;
	}

	/* store vg on disk(s) */
	if (!vg_write(vg)) {
		/* FIXME: Attempt reversion? */
		unlock_lv(cmd, lv->lvid.s);
		goto error;
	}

	backup(vg);

	if (!unlock_lv(cmd, lv->lvid.s)) {
		log_error("Problem reactivating %s", lv_name);
		goto error;
	}

	unlock_vg(cmd, vg_name);

	log_print("Logical volume %s successfully resized", lv_name);

	return 0;

      error:
	unlock_vg(cmd, vg_name);
	return ECMD_FAILED;

      error_cmdline:
	unlock_vg(cmd, vg_name);
	return EINVALID_CMD_LINE;
}
