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

static int _lvdisplay_colon_single(struct cmd_context *cmd, struct logical_volume *lv,
				   struct processing_handle *handle __attribute__ ((unused)))
{
	if (!arg_is_set(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	lvdisplay_colons(lv);

	return ECMD_PROCESSED;
}

static int _lvdisplay_general_single(struct cmd_context *cmd, struct logical_volume *lv,
				     struct processing_handle *handle __attribute__ ((unused)))
{
	if (!arg_is_set(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	lvdisplay_full(cmd, lv, NULL);

	if (arg_is_set(cmd, maps_ARG))
		lvdisplay_segments(lv);

	return ECMD_PROCESSED;
}

int lvdisplay_colon_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, argc, argv, NULL, NULL, 0, NULL, NULL, &_lvdisplay_colon_single);
}

int lvdisplay_general_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, argc, argv, NULL, NULL, 0, NULL, NULL, &_lvdisplay_general_single);
}

int lvdisplay_columns_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return lvs(cmd, argc, argv);
}

int lvdisplay(struct cmd_context *cmd, int argc, char **argv)
{
	log_error(INTERNAL_ERROR "Missing function for command definition %d:%s.",
		  cmd->command->command_index, command_enum(cmd->command->command_enum));
	return ECMD_FAILED;
}

