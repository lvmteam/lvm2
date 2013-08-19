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

static struct cft_check_handle *_get_cft_check_handle(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	struct cft_check_handle *handle;
	struct dm_pool *mem;

	if (cft == cmd->cft) {
		mem = cmd->libmem;
		handle = cmd->cft_check_handle;
	} else {
		mem = cft->mem;
		handle = NULL;
	}

	if (!handle) {
		if (!(handle = dm_pool_zalloc(mem, sizeof(struct cft_check_handle)))) {
			log_error("Configuration check handle allocation failed.");
			return NULL;
		}
		handle->cft = cft;
		if (cft == cmd->cft)
			cmd->cft_check_handle = handle;
	}

	return handle;
}

static int _do_def_check(struct cmd_context *cmd, struct dm_config_tree *cft,
			 struct cft_check_handle **cft_check_handle)
{
	struct cft_check_handle *handle;

	if (!(handle = _get_cft_check_handle(cmd, cft)))
		return 0;

	handle->force_check = 1;
	handle->skip_if_checked = 1;
	handle->suppress_messages = 1;

	config_def_check(cmd, handle);
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

int dumpconfig(struct cmd_context *cmd, int argc, char **argv)
{
	const char *file = arg_str_value(cmd, file_ARG, NULL);
	const char *type = arg_str_value(cmd, configtype_ARG, "current");
	struct config_def_tree_spec tree_spec = {0};
	struct dm_config_tree *cft = NULL;
	struct cft_check_handle *cft_check_handle = NULL;
	int r = ECMD_PROCESSED;

	if (arg_count(cmd, configtype_ARG) && arg_count(cmd, validate_ARG)) {
		log_error("Only one of --type and --validate permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, atversion_ARG) && !arg_count(cmd, configtype_ARG)) {
		log_error("--atversion requires --type");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, ignoreadvanced_ARG))
		tree_spec.ignoreadvanced = 1;

	if (arg_count(cmd, ignoreunsupported_ARG))
		tree_spec.ignoreunsupported = 1;

	if (!strcmp(type, "current")) {
		if (arg_count(cmd, atversion_ARG)) {
			log_error("--atversion has no effect with --type current");
			return EINVALID_CMD_LINE;
		}

		if ((tree_spec.ignoreadvanced || tree_spec.ignoreunsupported)) {
			log_error("--ignoreadvanced and --ignoreunsupported has "
				  "no effect with --type current");
			return EINVALID_CMD_LINE;
		}
	}

	if (!_get_vsn(cmd, &tree_spec.version))
		return EINVALID_CMD_LINE;

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
		if (!(cft_check_handle = _get_cft_check_handle(cmd, cft)))
			return ECMD_FAILED;

		cft_check_handle->force_check = 1;
		cft_check_handle->skip_if_checked = 1;
		cft_check_handle->suppress_messages = 0;

		if (config_def_check(cmd, cft_check_handle)) {
			log_print("LVM configuration valid.");
			goto out;
		} else {
			log_error("LVM configuration invalid.");
			r = ECMD_FAILED;
			goto out;
		}
	}

	if (!strcmp(type, "current")) {
		tree_spec.type = CFG_DEF_TREE_CURRENT;
		if (!_do_def_check(cmd, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "missing")) {
		tree_spec.type = CFG_DEF_TREE_MISSING;
		if (!_do_def_check(cmd, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "default")) {
		tree_spec.type = CFG_DEF_TREE_DEFAULT;
		/* default type does not require check status */
	}
	else if (!strcmp(type, "new")) {
		tree_spec.type = CFG_DEF_TREE_NEW;
		/* new type does not require check status */
	}
	else if (!strcmp(type, "profilable")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE;
		/* profilable type does not require check status */
	}
	else {
		log_error("Incorrect type of configuration specified. "
			  "Expected one of: current, default, missing, new, profilable.");
		r = EINVALID_CMD_LINE;
		goto out;
	}

	if (cft_check_handle)
		tree_spec.check_status = cft_check_handle->status;

	if ((tree_spec.type != CFG_DEF_TREE_CURRENT) &&
	    !(cft = config_def_create_tree(&tree_spec))) {
		r = ECMD_FAILED;
		goto_out;
	}

	if (!config_write(cft, arg_count(cmd, withcomments_ARG),
			  arg_count(cmd, withversions_ARG),
			  file, argc, argv)) {
		stack;
		r = ECMD_FAILED;
	}
out:
	if (cft && (cft != cmd->cft))
		dm_pool_destroy(cft->mem);

	/*
	 * The cmd->cft (the "current" tree) is destroyed
	 * together with cmd context destroy...
	 */

	return r;
}
