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

static int _pvdisplay_single(struct cmd_context *cmd,
			     struct volume_group *vg,
			     struct physical_volume *pv,
			     struct processing_handle *handle __attribute__((unused)))
{
	const char *pv_name = pv_dev_name(pv);
	int ret = ECMD_PROCESSED;
	uint64_t size;

	if (is_orphan(pv))
		size = pv_size(pv);
	else
		size = (uint64_t)(pv_pe_count(pv) - pv_pe_alloc_count(pv)) *
			pv_pe_size(pv);

	if (arg_is_set(cmd, short_ARG)) {
		log_print("Device \"%s\" has a capacity of %s", pv_name,
			  display_size(cmd, size));
		goto out;
	}

	if (pv_status(pv) & EXPORTED_VG)
		log_print_unless_silent("Physical volume \"%s\" of volume group \"%s\" "
					"is exported", pv_name, pv_vg_name(pv));

	if (is_orphan(pv))
		log_print_unless_silent("\"%s\" is a new physical volume of \"%s\"",
					pv_name, display_size(cmd, size));

	if (arg_is_set(cmd, colon_ARG))
		pvdisplay_colons(pv);
	else
		pvdisplay_full(cmd, pv, NULL);

	if (arg_is_set(cmd, maps_ARG))
		pvdisplay_segments(pv);

out:
	return ret;
}

int pvdisplay_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_pv(cmd, argc, argv, NULL, 0, 0, NULL, _pvdisplay_single);
}

int pvdisplay_columns_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	/*
	 * Without -a, command only looks at PVs and can use hints,
	 * with -a, the command looks at all (non-hinted) devices.
	 */
	if (arg_is_set(cmd, all_ARG))
		cmd->use_hints = 0;

	return pvs(cmd, argc, argv);
}

int pvdisplay(struct cmd_context *cmd, int argc, char **argv)
{
	log_error(INTERNAL_ERROR "Missing function for command definition %d:%s.",
		  cmd->command->command_index, command_enum(cmd->command->command_enum));
	return ECMD_FAILED;
}
