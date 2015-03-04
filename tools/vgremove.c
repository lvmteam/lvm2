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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

static int vgremove_single(struct cmd_context *cmd, const char *vg_name,
			   struct volume_group *vg,
			   struct processing_handle *handle __attribute__((unused)))
{
	/*
	 * Though vgremove operates per VG by definition, internally, it
	 * actually means iterating over each LV it contains to do the remove.
	 *
	 * Use processing handle with void_handle.internal_report_for_select=0
	 * for the process_each_lv_in_vg that is called later in this fn.
	 * We need to disable internal selection for process_each_lv_in_vg
	 * here as selection is already done by process_each_vg which calls
	 * vgremove_single. Otherwise selection would be done per-LV and
	 * not per-VG as we intend!
	 */
	struct processing_handle void_handle = {0};

	/*
	 * Single force is equivalent to sinle --yes
	 * Even multiple --yes are equivalent to single --force
	 * When we require -ff it cannot be replaces with -f -y
	 */
	force_t force = (force_t) arg_count(cmd, force_ARG)
		? : (arg_is_set(cmd, yes_ARG) ? DONT_PROMPT : PROMPT);
	unsigned lv_count, missing;
	int ret;

	if (!vg_check_status(vg, EXPORTED_VG))
		return_ECMD_FAILED;

	lv_count = vg_visible_lvs(vg);

	if (lv_count) {
		if (force == PROMPT) {
			if ((missing = vg_missing_pv_count(vg)))
				log_warn("WARNING: %d physical volumes are currently missing "
					 "from the system.", missing);
			if (yes_no_prompt("Do you really want to remove volume "
					  "group \"%s\" containing %u "
					  "logical volumes? [y/n]: ",
					  vg_name, lv_count) == 'n') {
				log_error("Volume group \"%s\" not removed", vg_name);
				return ECMD_FAILED;
			}
		}

		if ((ret = process_each_lv_in_vg(cmd, vg, NULL, NULL, 1, &void_handle,
						 (process_single_lv_fn_t)lvremove_single)) != ECMD_PROCESSED) {
			stack;
			return ret;
		}
	}

	if (!force && !vg_remove_check(vg))
		return_ECMD_FAILED;

	vg_remove_pvs(vg);

	if (!vg_remove(vg))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

int vgremove(struct cmd_context *cmd, int argc, char **argv)
{
	int ret;

	if (!argc && !arg_is_set(cmd, select_ARG)) {
		log_error("Please enter one or more volume group paths "
			  "or use --select for selection.");
		return EINVALID_CMD_LINE;
	}

	cmd->handles_missing_pvs = 1;
	ret = process_each_vg(cmd, argc, argv,
			      READ_FOR_UPDATE,
			      NULL, &vgremove_single);

	return ret;
}
