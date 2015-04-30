/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

static int _get_vsn(struct cmd_context *cmd, uint16_t *version_int)
{
	const char *atversion = arg_str_value(cmd, atversion_ARG, LVM_VERSION);
	unsigned int major, minor, patchlevel;

	if (sscanf(atversion, "%u.%u.%u", &major, &minor, &patchlevel) != 3) {
		log_error("Incorrect version format.");
		return 0;
	}

	*version_int = vsn(major, minor, patchlevel);
	return 1;
}

static int _do_def_check(struct config_def_tree_spec *spec,
			 struct dm_config_tree *cft,
			 struct cft_check_handle **cft_check_handle)
{
	struct cft_check_handle *handle;

	if (!(handle = get_config_tree_check_handle(spec->cmd, cft)))
		return 0;

	handle->force_check = 1;
	handle->suppress_messages = 1;

	if (spec->type == CFG_DEF_TREE_DIFF) {
		if (!handle->check_diff)
			handle->skip_if_checked = 0;
		handle->check_diff = 1;
	} else {
		handle->skip_if_checked = 1;
		handle->check_diff = 0;
	}

	handle->ignoreunsupported = spec->ignoreunsupported;
	handle->ignoreadvanced = spec->ignoreadvanced;

	config_def_check(handle);
	*cft_check_handle = handle;

	return 1;
}

static int _merge_config_cascade(struct cmd_context *cmd, struct dm_config_tree *cft_cascaded,
				 struct dm_config_tree **cft_merged)
{
	if (!cft_cascaded)
		return 1;

	if (!*cft_merged && !(*cft_merged = config_open(CONFIG_MERGED_FILES, NULL, 0)))
		return_0;

	if (!_merge_config_cascade(cmd, cft_cascaded->cascade, cft_merged))
		return_0;

	return merge_config_tree(cmd, *cft_merged, cft_cascaded, CONFIG_MERGE_TYPE_RAW);
}

static int _config_validate(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	struct cft_check_handle *handle;

	if (!(handle = get_config_tree_check_handle(cmd, cft)))
		return 1;

	handle->force_check = 1;
	handle->skip_if_checked = 1;
	handle->suppress_messages = 0;

	return config_def_check(handle);
}

int dumpconfig(struct cmd_context *cmd, int argc, char **argv)
{
	const char *file = arg_str_value(cmd, file_ARG, NULL);
	const char *type = arg_str_value(cmd, configtype_ARG, arg_count(cmd, list_ARG) ? "list" : "current");
	struct config_def_tree_spec tree_spec = {0};
	struct dm_config_tree *cft = NULL;
	struct cft_check_handle *cft_check_handle = NULL;
	struct profile *profile = NULL;
	int r = ECMD_PROCESSED;

	tree_spec.cmd = cmd;

	if (arg_count(cmd, configtype_ARG) && arg_count(cmd, validate_ARG)) {
		log_error("Only one of --type and --validate permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, configtype_ARG) && arg_count(cmd, list_ARG)) {
		log_error("Only one of --type and --list permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, atversion_ARG) && !arg_count(cmd, configtype_ARG) &&
	    !arg_count(cmd, list_ARG)) {
		log_error("--atversion requires --type or --list");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, ignoreadvanced_ARG))
		tree_spec.ignoreadvanced = 1;

	if (arg_count(cmd, ignoreunsupported_ARG)) {
		if (arg_count(cmd, showunsupported_ARG)) {
			log_error("Only one of --ignoreunsupported and --showunsupported permitted.");
			return EINVALID_CMD_LINE;
		}
		tree_spec.ignoreunsupported = 1;
	} else if (arg_count(cmd, showunsupported_ARG)) {
		tree_spec.ignoreunsupported = 0;
	} else if (strcmp(type, "current") && strcmp(type, "diff")) {
		/*
		 * By default hide unsupported settings
		 * for all display types except "current"
		 * and "diff".
		 */
		tree_spec.ignoreunsupported = 1;
	}

	if (strcmp(type, "current") && strcmp(type, "diff")) {
		/*
		 * By default hide deprecated settings
		 * for all display types except "current"
		 * and "diff" unless --showdeprecated is set.
		 *
		 * N.B. Deprecated settings are visible if
		 * --atversion is used with a version that
		 * is lower than the version in which the
		 * setting was deprecated.
		 */
		if (!arg_count(cmd, showdeprecated_ARG))
			tree_spec.ignoredeprecated = 1;
	}

	if (arg_count(cmd, ignorelocal_ARG))
		tree_spec.ignorelocal = 1;

	if (!strcmp(type, "current")) {
		if (arg_count(cmd, atversion_ARG)) {
			log_error("--atversion has no effect with --type current");
			return EINVALID_CMD_LINE;
		}

		if (arg_count(cmd, ignoreunsupported_ARG) ||
		    arg_count(cmd, ignoreadvanced_ARG)) {
			/* FIXME: allow these even for --type current */
			log_error("--ignoreadvanced and --ignoreunsupported has "
				  "no effect with --type current");
			return EINVALID_CMD_LINE;
		}
	} else if (arg_count(cmd, mergedconfig_ARG)) {
		log_error("--mergedconfig has no effect without --type current");
		return EINVALID_CMD_LINE;
	}

	if (!_get_vsn(cmd, &tree_spec.version))
		return EINVALID_CMD_LINE;

	/*
	 * The profile specified by --profile cmd arg is like --commandprofile,
	 * but it is used just for dumping the profile content and not for
	 * application.
	 */
	if (arg_count(cmd, profile_ARG) &&
	    (!(profile = add_profile(cmd, arg_str_value(cmd, profile_ARG, NULL), CONFIG_PROFILE_COMMAND)) ||
	    !override_config_tree_from_profile(cmd, profile))) {
		log_error("Failed to load profile %s.", arg_str_value(cmd, profile_ARG, NULL));
		return ECMD_FAILED;
	}

	/*
	 * Set the 'cft' to work with based on whether we need the plain
	 * config tree or merged config tree cascade if --mergedconfig is used.
	 */
	if (arg_count(cmd, mergedconfig_ARG) && cmd->cft->cascade) {
		if (!_merge_config_cascade(cmd, cmd->cft, &cft)) {
			log_error("Failed to merge configuration.");
			r = ECMD_FAILED;
			goto out;
		}
	} else
		cft = cmd->cft;

	if (arg_count(cmd, validate_ARG)) {
		if (_config_validate(cmd, cft)) {
			log_print("LVM configuration valid.");
			goto out;
		} else {
			log_error("LVM configuration invalid.");
			r = ECMD_FAILED;
			goto out;
		}
	}

	if (!strcmp(type, "list") || arg_count(cmd, list_ARG)) {
		tree_spec.type = CFG_DEF_TREE_LIST;
		if (arg_count(cmd, withcomments_ARG)) {
			log_error("--withcomments has no effect with --type list");
			return EINVALID_CMD_LINE;
		}
		/* list type does not require status check */
	} else if (!strcmp(type, "current")) {
		tree_spec.type = CFG_DEF_TREE_CURRENT;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "missing")) {
		tree_spec.type = CFG_DEF_TREE_MISSING;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "default")) {
		tree_spec.type = CFG_DEF_TREE_DEFAULT;
		/* default type does not require check status */
	}
	else if (!strcmp(type, "diff")) {
		tree_spec.type = CFG_DEF_TREE_DIFF;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "new")) {
		tree_spec.type = CFG_DEF_TREE_NEW;
		/* new type does not require check status */
	}
	else if (!strcmp(type, "profilable")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE;
		/* profilable type does not require check status */
	}
	else if (!strcmp(type, "profilable-command")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE_CMD;
		/* profilable-command type does not require check status */
	}
	else if (!strcmp(type, "profilable-metadata")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE_MDA;
		/* profilable-metadata  type does not require check status */
	}
	else {
		log_error("Incorrect type of configuration specified. "
			  "Expected one of: current, default, list, missing, new, "
			  "profilable, profilable-command, profilable-metadata.");
		r = EINVALID_CMD_LINE;
		goto out;
	}

	if (arg_count(cmd, withsummary_ARG) || arg_count(cmd, list_ARG))
		tree_spec.withsummary = 1;
	if (arg_count(cmd, withcomments_ARG))
		tree_spec.withcomments = 1;
	if (arg_count(cmd, unconfigured_ARG))
		tree_spec.unconfigured = 1;

	if (arg_count(cmd, withversions_ARG))
		tree_spec.withversions = 1;

	if (cft_check_handle)
		tree_spec.check_status = cft_check_handle->status;

	if ((tree_spec.type != CFG_DEF_TREE_CURRENT) &&
	    (tree_spec.type != CFG_DEF_TREE_DIFF) &&
	    !(cft = config_def_create_tree(&tree_spec))) {
		r = ECMD_FAILED;
		goto_out;
	}

	if (!config_write(cft, &tree_spec, file, argc, argv)) {
		stack;
		r = ECMD_FAILED;
	}
out:
	if (cft && (cft != cmd->cft))
		dm_pool_destroy(cft->mem);
	else if (profile)
		remove_config_tree_by_source(cmd, CONFIG_PROFILE_COMMAND);

	/*
	 * The cmd->cft (the "current" tree) is destroyed
	 * together with cmd context destroy...
	 */

	return r;
}

int config(struct cmd_context *cmd, int argc, char **argv)
{
	return dumpconfig(cmd, argc, argv);
}

int lvmconfig(struct cmd_context *cmd, int argc, char **argv)
{
	return dumpconfig(cmd, argc, argv);
}
