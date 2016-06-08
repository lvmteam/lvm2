/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
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
#include "lvmetad-client.h"

int vgcfgrestore(struct cmd_context *cmd, int argc, char **argv)
{
	const char *vg_name = NULL;
	int lvmetad_rescan = 0;
	int ret;

	if (argc == 1) {
		vg_name = skip_dev_dir(cmd, argv[0], NULL);
		if (!validate_name(vg_name)) {
			log_error("Volume group name \"%s\" is invalid", vg_name);
			return EINVALID_CMD_LINE;
		}
	} else if (!(arg_is_set(cmd, list_ARG) && arg_is_set(cmd, file_ARG))) {
		log_error("Please specify a *single* volume group to restore.");
		return EINVALID_CMD_LINE;
	}

	/*
	 * FIXME: overloading the -l arg for now to display a
	 * list of archive files for a particular vg
	 */
	if (arg_is_set(cmd, list_ARG)) {
		if (!(arg_is_set(cmd,file_ARG) ?
			    archive_display_file(cmd,
				arg_str_value(cmd, file_ARG, "")) :
			    archive_display(cmd, vg_name)))
			return_ECMD_FAILED;

		return ECMD_PROCESSED;
	}

	/*
	 * lvmetad does not handle a VG being restored, which would require
	 * vg_remove of the existing VG, then vg_update of the restored VG.  A
	 * command failure after removing the existing VG from lvmetad would
	 * not be easily recovered from.  So, disable the lvmetad cache before
	 * doing the restore.  After the VG is restored on disk, rescan
	 * metadata from disk to populate lvmetad from scratch which will pick
	 * up the VG that was restored on disk.
	 */

	if (lvmetad_used()) {
		lvmetad_set_disabled(cmd, LVMETAD_DISABLE_REASON_VGRESTORE);
		lvmetad_disconnect();
		lvmetad_rescan = 1;
	}

	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE, NULL)) {
		log_error("Unable to lock volume group %s", vg_name);
		return ECMD_FAILED;
	}

	if (!lock_vol(cmd, VG_ORPHANS, LCK_VG_WRITE, NULL)) {
		log_error("Unable to lock orphans");
		unlock_vg(cmd, NULL, vg_name);
		return ECMD_FAILED;
	}

	cmd->handles_unknown_segments = 1;

	if (!(arg_is_set(cmd, file_ARG) ?
	      backup_restore_from_file(cmd, vg_name,
				       arg_str_value(cmd, file_ARG, ""),
				       arg_count(cmd, force_long_ARG)) :
	      backup_restore(cmd, vg_name, arg_count(cmd, force_long_ARG)))) {
		unlock_vg(cmd, NULL, VG_ORPHANS);
		unlock_vg(cmd, NULL, vg_name);
		log_error("Restore failed.");
		ret = ECMD_FAILED;
		goto rescan;
	}

	ret = ECMD_PROCESSED;
	log_print_unless_silent("Restored volume group %s", vg_name);

	unlock_vg(cmd, NULL, VG_ORPHANS);
	unlock_vg(cmd, NULL, vg_name);
rescan:
	if (lvmetad_rescan) {
		if (!lvmetad_connect(cmd)) {
			log_warn("WARNING: Failed to connect to lvmetad.");
			log_warn("WARNING: Update lvmetad with pvscan --cache.");
			goto out;
		}
		if (!refresh_filters(cmd))
			stack;
		if (!lvmetad_pvscan_all_devs(cmd, 1)) {
			log_warn("WARNING: Failed to scan devices.");
			log_warn("WARNING: Update lvmetad with pvscan --cache.");
			goto out;
		}
	}
out:
	return ret;
}
