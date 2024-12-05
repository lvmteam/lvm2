/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"

static int _vgdisplay_colon_single(struct cmd_context *cmd, const char *vg_name,
			     struct volume_group *vg,
			     struct processing_handle *handle __attribute__((unused)))
{
	if (arg_is_set(cmd, activevolumegroups_ARG) && !lvs_in_vg_activated(vg))
		return ECMD_PROCESSED;

	vgdisplay_colons(vg);

	return ECMD_PROCESSED;
}

static int _vgdisplay_general_single(struct cmd_context *cmd, const char *vg_name,
			     struct volume_group *vg,
			     struct processing_handle *handle __attribute__((unused)))
{
	if (arg_is_set(cmd, activevolumegroups_ARG) && !lvs_in_vg_activated(vg))
		return ECMD_PROCESSED;

	if (arg_is_set(cmd, short_ARG)) {
		vgdisplay_short(vg);
		return ECMD_PROCESSED;
	}

	vgdisplay_full(vg);

	if (arg_is_set(cmd, verbose_ARG)) {
		vgdisplay_extents(vg);

		process_each_lv_in_vg(cmd, vg, NULL, NULL, 0, NULL,
				      NULL, (process_single_lv_fn_t)lvdisplay_full);

		log_print("--- Physical volumes ---");
		process_each_pv_in_vg(cmd, vg, NULL,
				      (process_single_pv_fn_t)pvdisplay_short);
	}

	check_current_backup(vg);

	return ECMD_PROCESSED;
}

int vgdisplay_colon_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	if (argc && arg_is_set(cmd, activevolumegroups_ARG)) {
		log_error("Option -A is not allowed with volume group names");
		return EINVALID_CMD_LINE;
	}

	return process_each_vg(cmd, argc, argv, NULL, NULL, 0, 0, NULL, _vgdisplay_colon_single);
}

int vgdisplay_general_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	if (argc && arg_is_set(cmd, activevolumegroups_ARG)) {
		log_error("Option -A is not allowed with volume group names");
		return EINVALID_CMD_LINE;
	}

	return process_each_vg(cmd, argc, argv, NULL, NULL, 0, 0, NULL, _vgdisplay_general_single);
}

int vgdisplay_columns_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return vgs(cmd, argc, argv);
}

int vgdisplay(struct cmd_context *cmd, int argc, char **argv)
{
	log_error(INTERNAL_ERROR "Missing function for command definition %d:%s.",
		  cmd->command->command_index, command_enum(cmd->command->command_enum));
	return ECMD_FAILED;
}

