/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

int vgcfgrestore(struct cmd_context *cmd, int argc, char **argv)
{
	if (argc != 1) {
		log_err("Please specify a *single* volume group to restore.");
		return ECMD_FAILED;
	}

	/*
	 * FIXME: overloading the -l arg for now to display a
	 * list of archive files for a particular vg
	 */
	if (arg_count(cmd, list_ARG)) {
		if (!archive_display(cmd, argv[0]))
			return ECMD_FAILED;

		return 0;
	}

	if (!lock_vol(cmd, ORPHAN, LCK_VG_WRITE)) {
		log_error("Unable to lock orphans");
		return ECMD_FAILED;
	}

	if (!lock_vol(cmd, argv[0], LCK_VG_WRITE | LCK_NONBLOCK)) {
		log_error("Unable to lock volume group %s", argv[0]);
		unlock_vg(cmd, ORPHAN);
		return ECMD_FAILED;
	}

	if (!(arg_count(cmd, file_ARG) ?
	      backup_restore_from_file(cmd, argv[0],
				       arg_str_value(cmd, file_ARG, "")) :
	      backup_restore(cmd, argv[0]))) {
		unlock_vg(cmd, argv[0]);
		unlock_vg(cmd, ORPHAN);
		log_err("Restore failed.");
		return ECMD_FAILED;
	}

	unlock_vg(cmd, argv[0]);
	unlock_vg(cmd, ORPHAN);
	return 0;
}
