/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
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

int lvremove(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle = NULL;
	struct lvremove_params lp = {
		.removed_uuids = DM_LIST_HEAD_INIT(lp.removed_uuids),
	};
	int ret;

	if (!argc && !arg_is_set(cmd, select_ARG)) {
		log_error("Please enter one or more logical volume paths "
			  "or use --select for selection.");
		return EINVALID_CMD_LINE;
	}

	cmd->handles_missing_pvs = 1;
	cmd->include_historical_lvs = 1;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lp;

	ret = process_each_lv(cmd, argc, argv, NULL, NULL, READ_FOR_UPDATE, handle,
			       NULL, &lvremove_single);

	if (cmd->scan_lvs && cmd->enable_devices_file)
		device_id_lvremove(cmd, &lp.removed_uuids);

	destroy_processing_handle(cmd, handle);

	return ret;
}
