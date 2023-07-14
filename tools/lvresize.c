/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2016 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"

static int _lvresize_params(struct cmd_context *cmd, struct lvresize_params *lp)
{
	const char *type_str = arg_str_value(cmd, type_ARG, NULL);
	int only_linear = 0;
	int set_fsopt = 0;
	int set_extents_and_size = 0;

	memset(lp, 0, sizeof(struct lvresize_params));

	switch (cmd->command->command_enum) {
	case lvextend_policy_CMD:
		lp->resize = LV_EXTEND;
		lp->size = 0;
		lp->extents = 0;
		lp->percent = PERCENT_LV;
		lp->sign = SIGN_PLUS;
		lp->poolmetadata_size = 0;
		lp->use_policies = 1;
		break;

	case lvextend_pool_metadata_CMD:
	case lvresize_pool_metadata_CMD:
		lp->resize = LV_EXTEND;
		lp->size = 0;
		lp->extents = 0;
		lp->percent = PERCENT_NONE;
		lp->sign = SIGN_NONE;
		lp->poolmetadata_size = arg_uint64_value(cmd, poolmetadatasize_ARG, 0);
		lp->poolmetadata_sign = arg_sign_value(cmd, poolmetadatasize_ARG, SIGN_NONE);
		break;

	case lvextend_pv_CMD:
	case lvresize_pv_CMD:
		lp->resize = LV_EXTEND;
		lp->size = 0;
		lp->extents = 0;
		lp->percent_value = 100;
		lp->percent = PERCENT_PVS;
		lp->sign = SIGN_PLUS;
		lp->poolmetadata_size = 0;
		set_fsopt = 1;
		break;

	case lvextend_size_CMD:
		lp->resize = LV_EXTEND;
		if ((lp->poolmetadata_size = arg_uint64_value(cmd, poolmetadatasize_ARG, 0)))
			lp->poolmetadata_sign = arg_sign_value(cmd, poolmetadatasize_ARG, SIGN_NONE);
		set_extents_and_size = 1;
		set_fsopt = 1;
		break;

	case lvreduce_size_CMD:
		lp->resize = LV_REDUCE;
		lp->poolmetadata_size = 0;
		set_extents_and_size = 1;
		set_fsopt = 1;
		break;

	case lvresize_size_CMD:
		lp->resize = LV_ANY;
		lp->poolmetadata_size = arg_uint64_value(cmd, poolmetadatasize_ARG, 0);
		if ((lp->poolmetadata_size = arg_uint64_value(cmd, poolmetadatasize_ARG, 0)))
			lp->poolmetadata_sign = arg_sign_value(cmd, poolmetadatasize_ARG, SIGN_NONE);
		set_extents_and_size = 1;
		set_fsopt = 1;
		break;

	default:
		log_error(INTERNAL_ERROR "unknown lvresize type");
		return 0;
	};

	if (set_fsopt) {
		const char *str;

		if (arg_is_set(cmd, resizefs_ARG) && arg_is_set(cmd, fs_ARG)) {
			log_error("Options --fs and --resizefs cannot be used together.");
			log_error("--resizefs is equivalent to --fs resize.");
			return 0;
		}

#ifdef HAVE_BLKID_SUBLKS_FSINFO
		/*
		 * When the libblkid fs info feature is available, use the
		 * the newer fs resizing capabability unless the older
		 * fsadm-based resizing is requested with --fs resize_fsadm.
		 */
		if ((str = arg_str_value(cmd, fs_ARG, NULL))) {
			if (!strcmp(str, "checksize") ||
			    !strcmp(str, "resize") ||
			    !strcmp(str, "resize_fsadm")) {
				strncpy(lp->fsopt, str, sizeof(lp->fsopt)-1);
			} else if (!strcmp(str, "ignore")) {
				lp->fsopt[0] = '\0';
			} else {
				log_error("Unknown --fs value.");
				return 0;
			}
			lp->user_set_fs = 1;
		} else if (arg_is_set(cmd, resizefs_ARG)) {
			/* --resizefs alone equates to --fs resize */
			strncpy(lp->fsopt, "resize", sizeof(lp->fsopt)-1);
			lp->user_set_fs = 1;
		} else {
			/*
			 * Use checksize when no fs option is specified.
			 * checksize with extend does nothing: the LV
			 * is extended and any fs is ignored.
			 * checksize with reduce checks for an fs that
			 * needs reducing: the LV is reduced only if the
			 * fs does not need to be reduced (or no fs.)
			 */
			strncpy(lp->fsopt, "checksize", sizeof(lp->fsopt)-1);
		}
#else
		/*
		 * When the libblkid fs info feature is not available we can only
		 * use fsadm, so --resizefs, --fs resize, --fs resize_fsadm
		 * all translate to resize_fsadm.
		 */
		if ((str = arg_str_value(cmd, fs_ARG, NULL))) {
			if (!strcmp(str, "resize")) {
				log_warn("Using fsadm for file system handling (resize_fsadm).");
				strcpy(lp->fsopt, "resize_fsadm");
			} else if (!strcmp(str, "resize_fsadm")) {
				strcpy(lp->fsopt, "resize_fsadm");
			} else if (!strcmp(str, "ignore")) {
				log_warn("Ignoring unsupported --fs ignore with fsadm resizing.");
			} else {
				log_error("Unknown --fs value.");
				return 0;
			}
		} else if (arg_is_set(cmd, resizefs_ARG)) {
			/* --resizefs alone equates to --fs resize_fsadm */
			strcpy(lp->fsopt, "resize_fsadm");
		}
#endif
		if (lp->fsopt[0])
			lp->nofsck = arg_is_set(cmd, nofsck_ARG);

		if (!strcmp(lp->fsopt, "resize_fsadm") && arg_is_set(cmd, fsmode_ARG)) {
			log_error("The --fsmode option does not apply to resize_fsadm.");
			return 0;
		}

		if ((str = arg_str_value(cmd, fsmode_ARG, NULL))) {
			if (!strcmp(str, "nochange") ||
			    !strcmp(str, "offline") ||
			    !strcmp(str, "manage")) {
				strncpy(lp->fsmode, str, sizeof(lp->fsmode)-1);
				lp->user_set_fsmode = 1;
			} else {
				log_error("Unknown --fsmode value.");
				return 0;
			}
		} else {
			/* Use manage when no fsmode option is specified. */
			strncpy(lp->fsmode, "manage", sizeof(lp->fsmode)-1);
		}
	}

	if (set_extents_and_size) {
		if ((lp->extents = arg_uint_value(cmd, extents_ARG, 0))) {
			lp->sign = arg_sign_value(cmd, extents_ARG, 0);
			lp->percent = arg_percent_value(cmd, extents_ARG, PERCENT_NONE);
		}
		if ((lp->size = arg_uint64_value(cmd, size_ARG, 0))) {
			lp->sign = arg_sign_value(cmd, size_ARG, 0);
			lp->percent = PERCENT_NONE;
		}
		if (lp->size && lp->extents) {
			log_error("Please specify either size or extents but not both.");
			return 0;
		}
	}

	lp->alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, 0);
	lp->yes = arg_is_set(cmd, yes_ARG);
	lp->force = arg_is_set(cmd, force_ARG),
	lp->nosync = arg_is_set(cmd, nosync_ARG);
	lp->lockopt = arg_str_value(cmd, lockopt_ARG, NULL);

	if (type_str) {
		if (!strcmp(type_str, "linear")) {
			type_str = "striped";
			only_linear = 1; /* User requested linear only target */
		}

		if (!(lp->segtype = get_segtype_from_string(cmd, type_str)))
			return_0;
	}

	if ((lp->resize == LV_REDUCE) &&
	    (type_str ||
	     arg_is_set(cmd, mirrors_ARG) ||
	     arg_is_set(cmd, stripes_ARG) ||
	     arg_is_set(cmd, stripesize_ARG))) {
		/* should be obvious since reduce doesn't alloc space. */
		log_print_unless_silent("Ignoring type, stripes, stripesize and mirrors "
					"arguments when reducing.");
		goto out;
	}

	if (arg_is_set(cmd, mirrors_ARG)) {
		if (arg_sign_value(cmd, mirrors_ARG, SIGN_NONE) != SIGN_NONE) {
			log_error("Mirrors argument may not be signed.");
			return 0;
		}
		if ((lp->mirrors = arg_uint_value(cmd, mirrors_ARG, 0)))
			lp->mirrors++;
	}

	if ((lp->stripes = arg_uint_value(cmd, stripes_ARG, 0)) &&
	    (arg_sign_value(cmd, stripes_ARG, SIGN_NONE) == SIGN_MINUS)) {
		log_error("Stripes argument may not be negative.");
		return 0;
	}

	if (only_linear && lp->stripes > 1) {
		log_error("Cannot use stripes with linear type.");
		return 0;
	}

	if ((lp->stripe_size = arg_uint64_value(cmd, stripesize_ARG, 0)) &&
	    (arg_sign_value(cmd, stripesize_ARG, SIGN_NONE) == SIGN_MINUS)) {
		log_error("Stripesize may not be negative.");
		return 0;
	}
out:
	return 1;
}

/*
 * lvextend --use-policies is usually called by dmeventd, as a method of
 * "auto extending" an LV as it's used.  It checks how full a snapshot cow or
 * thin pool is, and extends it if it's too full, based on threshold settings
 * in lvm.conf for when to auto extend it.
 *
 * The extension of a thin pool LV can involve extending either the data sub
 * LV, the metadata sub LV, or both, so there may be two LVs extended here.
 */
static int _lv_extend_policy(struct cmd_context *cmd, struct logical_volume *lv,
			     struct lvresize_params *lp, int *skipped)
{
	uint32_t percent_main = 0;
	uint32_t percent_meta = 0;
	int is_active;

	if (lv_is_cow(lv))
		is_active = lv_is_active(lv);
	else if (lv_is_thin_pool(lv) || lv_is_vdo_pool(lv))
		/* check for -layer active LV */
		is_active = lv_info(lv->vg->cmd, lv, 1, NULL, 0, 0);
	else {
		log_error("lvextend policy is supported only for snapshot, thin pool and vdo pool volumes.");
		return 0;
	}

	if (!is_active) {
		log_error("lvextend using policy requires the volume to be active.");
		return 0;
	}

	/*
	 * Calculate the percent of extents to extend the LV based on current
	 * usage info from the kernel and policy settings from lvm.conf, e.g.
	 * autoextend_threshold, autoextend_percent.  For thin pools, both the
	 * thin pool data LV and thin pool metadata LV may need to be extended.
	 * In this case, percent_main is the amount to extend the data LV, and
	 * percent_meta is the amount to extend the metadata LV.
	 */
	if (!lv_extend_policy_calculate_percent(lv, &percent_main, &percent_meta))
		return_0;

	if (!percent_main && !percent_meta) {
		log_debug("lvextend policy not needed.");
		*skipped = 1;
		return 1;
	}

	*skipped = 0;
	lp->policy_percent_main = percent_main;
	lp->policy_percent_meta = percent_meta;

	return lv_resize(cmd, lv, lp);
}

static int _lvextend_policy_single(struct cmd_context *cmd, struct logical_volume *lv,
				   struct processing_handle *handle)
{
	struct lvresize_params *lp = (struct lvresize_params *) handle->custom_handle;
	int skipped = 0;

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(lp->pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
	} else
		lp->pvh = &lv->vg->pvs;

	if (!_lv_extend_policy(cmd, lv, lp, &skipped))
		return ECMD_FAILED;

	if (!skipped)
		log_print_unless_silent("Logical volume %s successfully resized.", display_lvname(lv));
	return ECMD_PROCESSED;
}

static int _lvresize_single(struct cmd_context *cmd, struct logical_volume *lv,
			    struct processing_handle *handle)
{
	struct lvresize_params *lp = (struct lvresize_params *) handle->custom_handle;
	int ret;

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(lp->pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
	} else
		lp->pvh = &lv->vg->pvs;

	ret = lv_resize(cmd, lv, lp);

	if (ret || lp->extend_fs_error)
		log_print_unless_silent("Logical volume %s successfully resized.",
					display_lvname(lv));
	if (!ret)
		return ECMD_FAILED;
	return ECMD_PROCESSED;
}

int lvextend_policy_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct lvresize_params lp;
	int ret;

	if (!_lvresize_params(cmd, &lp))
		return EINVALID_CMD_LINE;

	if (!(handle = init_processing_handle(cmd, NULL)))
		return ECMD_FAILED;

	handle->custom_handle = &lp;

	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, NULL, &_lvextend_policy_single);

	destroy_processing_handle(cmd, handle);

	return ret;
}

int lvresize_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct lvresize_params lp;
	int retries = 0;
	int ret;

	if (!_lvresize_params(cmd, &lp))
		return EINVALID_CMD_LINE;

	if (!(handle = init_processing_handle(cmd, NULL)))
		return ECMD_FAILED;

	handle->custom_handle = &lp;

retry:
	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, NULL, &_lvresize_single);

	/*
	 * The VG can be changed by another command while it is unlocked
	 * during fs resize.  The fs steps likely succeeded, and this
	 * retry will likely find that no more fs steps are needed, and
	 * will resize the LV directly.
	 */
	if (lp.vg_changed_error && !retries) {
		lp.vg_changed_error = 0;
		retries = 1;
		goto retry;
	} else if (lp.vg_changed_error && retries) {
		log_error("VG changed during file system resize, LV not resized.");
		ret = ECMD_FAILED;
	}

	destroy_processing_handle(cmd, handle);

	if (lp.lockd_lv_refresh_path && !lockd_lv_refresh(cmd, &lp))
		ret = ECMD_FAILED;

	return ret;
}

/*
 * All lvresize command defs have their own function,
 * so the generic function name is unused.
 */

int lvresize(struct cmd_context *cmd, int argc, char **argv)
{
	log_error(INTERNAL_ERROR "Missing function for command definition %d:%s.",
		  cmd->command->command_index, cmd->command->command_id);
	return ECMD_FAILED;
}

