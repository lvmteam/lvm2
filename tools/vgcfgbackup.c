/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

static int vg_backup_single(struct cmd_context *cmd, const char *vg_name,
			    struct volume_group *vg, int consistent,
			    void *handle)
{
	if (!vg) {
		log_error("Volume group \"%s\" not found", vg_name);
		return ECMD_FAILED;
	}

	if (!consistent)
		log_error("Warning: Volume group \"%s\" inconsistent", vg_name);

	if (arg_count(cmd, file_ARG)) {
		backup_to_file(arg_value(cmd, file_ARG), vg->cmd->cmd_line, vg);

	} else {
		if (!consistent) {
			log_error("No backup taken: specify filename with -f "
				  "to backup an inconsistent VG");
			stack;
			return ECMD_FAILED;
		}

		/* just use the normal backup code */
		backup_enable(1);	/* force a backup */
		if (!backup(vg)) {
			stack;
			return ECMD_FAILED;
		}
	}

	log_print("Volume group \"%s\" successfully backed up.", vg_name);
	return ECMD_PROCESSED;
}

int vgcfgbackup(struct cmd_context *cmd, int argc, char **argv)
{
	int ret;

	if (partial_mode())
		init_pvmove(1);

	ret = process_each_vg(cmd, argc, argv, LCK_VG_READ, 0, NULL,
			      &vg_backup_single);

	init_pvmove(0);

	return ret;
}
