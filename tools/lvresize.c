/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

static int _lvresize_params(struct cmd_context *cmd, int argc, char **argv,
			    struct lvresize_params *lp)
{
	const char *cmd_name;
	char *st;
	unsigned dev_dir_found = 0;
	int use_policy = arg_count(cmd, use_policies_ARG);

	lp->sign = SIGN_NONE;
	lp->poolmetadatasign = SIGN_NONE;
	lp->resize = LV_ANY;

	cmd_name = command_name(cmd);
	if (!strcmp(cmd_name, "lvreduce"))
		lp->resize = LV_REDUCE;
	if (!strcmp(cmd_name, "lvextend"))
		lp->resize = LV_EXTEND;

	if (use_policy) {
		/* do nothing; _lvresize will handle --use-policies itself */
		lp->extents = 0;
		lp->sign = SIGN_PLUS;
		lp->percent = PERCENT_LV;
	} else {
		/*
		 * Allow omission of extents and size if the user has given us
		 * one or more PVs.  Most likely, the intent was "resize this
		 * LV the best you can with these PVs"
		 * If only --poolmetadatasize is specified with list of PVs,
		 * then metadata will be extended there.
		 */
		lp->sizeargs = arg_count(cmd, extents_ARG) + arg_count(cmd, size_ARG);
		if ((lp->sizeargs == 0) && (argc >= 2)) {
			lp->extents = 100;
			lp->percent = PERCENT_PVS;
			lp->sign = SIGN_PLUS;
			lp->sizeargs = !lp->poolmetadatasize ? 1 : 0;
		} else if ((lp->sizeargs != 1) &&
			   ((lp->sizeargs == 2) ||
			    !arg_count(cmd, poolmetadatasize_ARG))) {
			log_error("Please specify either size or extents but not "
				  "both.");
			return 0;
		}

		if (arg_count(cmd, extents_ARG)) {
			lp->extents = arg_uint_value(cmd, extents_ARG, 0);
			lp->sign = arg_sign_value(cmd, extents_ARG, SIGN_NONE);
			lp->percent = arg_percent_value(cmd, extents_ARG, PERCENT_NONE);
		}

		/* Size returned in kilobyte units; held in sectors */
		if (arg_count(cmd, size_ARG)) {
			lp->size = arg_uint64_value(cmd, size_ARG, 0);
			lp->sign = arg_sign_value(cmd, size_ARG, SIGN_NONE);
			lp->percent = PERCENT_NONE;
		}

		if (arg_count(cmd, poolmetadatasize_ARG)) {
			lp->poolmetadatasize = arg_uint64_value(cmd, poolmetadatasize_ARG, 0);
			lp->poolmetadatasign = arg_sign_value(cmd, poolmetadatasize_ARG, SIGN_NONE);
			if (lp->poolmetadatasign == SIGN_MINUS) {
				log_error("Can't reduce pool metadata size.");
				return 0;
			}
		}
	}

	if (lp->resize == LV_EXTEND && lp->sign == SIGN_MINUS) {
		log_error("Negative argument not permitted - use lvreduce");
		return 0;
	}

	if (lp->resize == LV_REDUCE &&
	    ((lp->sign == SIGN_PLUS) || (lp->poolmetadatasign == SIGN_PLUS))) {
		log_error("Positive sign not permitted - use lvextend");
		return 0;
	}

	lp->resizefs = arg_is_set(cmd, resizefs_ARG);
	lp->nofsck = arg_is_set(cmd, nofsck_ARG);

	if (!argc) {
		log_error("Please provide the logical volume name");
		return 0;
	}

	lp->lv_name = argv[0];
	argv++;
	argc--;

	if (!(lp->lv_name = skip_dev_dir(cmd, lp->lv_name, &dev_dir_found)) ||
	    !(lp->vg_name = extract_vgname(cmd, lp->lv_name))) {
		log_error("Please provide a volume group name");
		return 0;
	}

	if (!validate_name(lp->vg_name)) {
		log_error("Volume group name %s has invalid characters",
			  lp->vg_name);
		return 0;
	}

	if ((st = strrchr(lp->lv_name, '/')))
		lp->lv_name = st + 1;

	lp->argc = argc;
	lp->argv = argv;

	lp->ac_policy = arg_count(cmd, use_policies_ARG);
	lp->ac_stripes = arg_count(cmd, stripes_ARG);
	if (lp->ac_stripes) {
		lp->ac_stripes_value = arg_uint_value(cmd, stripes_ARG, 1);
	} else {
		lp->ac_stripes_value = 0;
	}

	lp->ac_mirrors = arg_count(cmd, mirrors_ARG);

	if (lp->ac_mirrors) {
		if (arg_sign_value(cmd, mirrors_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Mirrors argument may not be negative");
			return 0;
		}

		lp->ac_mirrors_value = arg_uint_value(cmd, mirrors_ARG, 1) + 1;
	} else {
		lp->ac_mirrors_value = 0;
	}

	lp->ac_stripesize = arg_count(cmd, stripesize_ARG);
	if (lp->ac_stripesize) {
		if (arg_sign_value(cmd, stripesize_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Stripesize may not be negative.");
			return 0;
		}

		lp->ac_stripesize_value = arg_uint64_value(cmd, stripesize_ARG, 0);
	}

	lp->ac_no_sync = arg_count(cmd, nosync_ARG);
	lp->ac_alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, 0);

	lp->ac_type = arg_str_value(cmd, type_ARG, NULL);
	lp->ac_force = arg_count(cmd, force_ARG);

	return 1;
}

int lvresize(struct cmd_context *cmd, int argc, char **argv)
{
	struct lvresize_params lp = { 0 };
	struct volume_group *vg;
	struct dm_list *pvh = NULL;
	struct lv_list *lvl;
	int r = ECMD_FAILED;

	if (!_lvresize_params(cmd, argc, argv, &lp))
		return EINVALID_CMD_LINE;

	log_verbose("Finding volume group %s", lp.vg_name);
	vg = vg_read_for_update(cmd, lp.vg_name, NULL, 0);
	if (vg_read_error(vg)) {
		release_vg(vg);
		return_ECMD_FAILED;
	}

        /* Does LV exist? */
        if (!(lvl = find_lv_in_vg(vg, lp.lv_name))) {
                log_error("Logical volume %s not found in volume group %s",
                          lp.lv_name, lp.vg_name);
		goto out;
        }

	if (!(pvh = lp.argc ? create_pv_list(cmd->mem, vg, lp.argc,
					     lp.argv, 1) : &vg->pvs))
		goto_out;

	if (!lv_resize_prepare(cmd, lvl->lv, &lp, pvh)) {
		r = EINVALID_CMD_LINE;
		goto_out;
	}

	if (!lv_resize(cmd, lvl->lv, &lp, pvh))
		goto_out;

	r = ECMD_PROCESSED;

out:
	unlock_and_release_vg(cmd, vg, lp.vg_name);

	return r;
}
