/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

int vgcfgrestore(struct cmd_context *cmd, int argc, char **argv)
{
	char *vg_name;

	if (argc != 1) {
		log_err("Please specify a *single* volume group to restore.");
		return ECMD_FAILED;
	}

	vg_name = argv[0];

	if (!strncmp(vg_name, cmd->dev_dir, strlen(cmd->dev_dir)))
		vg_name += strlen(cmd->dev_dir);

	if (!validate_name(vg_name)) {
		log_error("Volume group name \"%s\" has invalid characters",
			  vg_name);
		return ECMD_FAILED;
	}

	/*
	 * FIXME: overloading the -l arg for now to display a
	 * list of archive files for a particular vg
	 */
	if (arg_count(cmd, list_ARG)) {
		if (!archive_display(cmd, vg_name))
			return ECMD_FAILED;

		return 0;
	}

	if (!lock_vol(cmd, ORPHAN, LCK_VG_WRITE)) {
		log_error("Unable to lock orphans");
		return ECMD_FAILED;
	}

	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE | LCK_NONBLOCK)) {
		log_error("Unable to lock volume group %s", vg_name);
		unlock_vg(cmd, ORPHAN);
		return ECMD_FAILED;
	}

	if (!(arg_count(cmd, file_ARG) ?
	      backup_restore_from_file(cmd, vg_name,
				       arg_str_value(cmd, file_ARG, "")) :
	      backup_restore(cmd, vg_name))) {
		unlock_vg(cmd, vg_name);
		unlock_vg(cmd, ORPHAN);
		log_err("Restore failed.");
		return ECMD_FAILED;
	}

	log_print("Restored volume group %s", vg_name);

	unlock_vg(cmd, vg_name);
	unlock_vg(cmd, ORPHAN);
	return 0;
}
