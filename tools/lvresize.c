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
		lp->resizefs = arg_is_set(cmd, resizefs_ARG);
		lp->nofsck = arg_is_set(cmd, nofsck_ARG);
		break;

	case lvextend_size_CMD:
		lp->resize = LV_EXTEND;
		lp->resizefs = arg_is_set(cmd, resizefs_ARG);
		lp->nofsck = arg_is_set(cmd, nofsck_ARG);
		if ((lp->poolmetadata_size = arg_uint64_value(cmd, poolmetadatasize_ARG, 0)))
			lp->poolmetadata_sign = arg_sign_value(cmd, poolmetadatasize_ARG, SIGN_NONE);
		set_extents_and_size = 1;
		break;

	case lvreduce_size_CMD:
		lp->resize = LV_REDUCE;
		lp->poolmetadata_size = 0;
		lp->resizefs = arg_is_set(cmd, resizefs_ARG);
		lp->nofsck = arg_is_set(cmd, nofsck_ARG);
		set_extents_and_size = 1;
		break;

	case lvresize_size_CMD:
		lp->resize = LV_ANY;
		lp->poolmetadata_size = arg_uint64_value(cmd, poolmetadatasize_ARG, 0);
		lp->resizefs = arg_is_set(cmd, resizefs_ARG);
		lp->nofsck = arg_is_set(cmd, nofsck_ARG);
		if ((lp->poolmetadata_size = arg_uint64_value(cmd, poolmetadatasize_ARG, 0)))
			lp->poolmetadata_sign = arg_sign_value(cmd, poolmetadatasize_ARG, SIGN_NONE);
		set_extents_and_size = 1;
		break;

	default:
		log_error(INTERNAL_ERROR "unknown lvresize type");
		return 0;
	};

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
	struct lvresize_params lp_meta;
	uint32_t percent_main = 0;
	uint32_t percent_meta = 0;
	int is_active;

	memset(&lp_meta, 0, sizeof(lp_meta));

	if (!lv_is_cow(lv) && !lv_is_thin_pool(lv) && !lv_is_vdo_pool(lv)) {
		log_error("lvextend policy is supported only for snapshot, thin pool and vdo pool volumes.");
		*skipped = 1;
		return 0;
	}

	is_active = lv_is_active(lv);

	if (vg_is_shared(lv->vg) && !is_active) {
		log_debug("lvextend policy requires LV to be active in a shared VG.");
		*skipped = 1;
		return 1;
	}

	if (lv_is_thin_pool(lv) && !is_active) {
		log_error("lvextend using policy requires the thin pool to be active.");
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

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(lp->pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
	} else
		lp->pvh = &lv->vg->pvs;

	if (!lv_resize(cmd, lv, lp))
		return ECMD_FAILED;

	log_print_unless_silent("Logical volume %s successfully resized.",
				display_lvname(lv));
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
	int ret;

	if (!_lvresize_params(cmd, &lp))
		return EINVALID_CMD_LINE;

	if (!(handle = init_processing_handle(cmd, NULL)))
		return ECMD_FAILED;

	handle->custom_handle = &lp;

	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, NULL, &_lvresize_single);

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

