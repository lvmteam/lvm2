/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

#include <stdio.h>

static int _backup_to_file(const char *file, struct volume_group *vg)
{
	int r;
	struct format_instance *tf;

	if (!(tf = text_format_create(vg->cmd, file, the_um,
				      vg->cmd->cmd_line))) {
		log_error("Couldn't create backup object.");
		return 0;
	}

	if (!(r = tf->ops->vg_write(tf, vg)))
		stack;

	tf->ops->destroy(tf);
	return r;
}

static int vg_backup_single(const char *vg_name)
{
	struct volume_group *vg;

	log_verbose("Checking for volume group \"%s\"", vg_name);
	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Volume group \"%s\" not found", vg_name);
		return ECMD_FAILED;
	}

	if (arg_count(file_ARG)) {
		_backup_to_file(arg_value(file_ARG), vg);

	} else {
		/* just use the normal backup code */
		backup_enable(1);	/* force a backup */
		if (!backup(vg)) {
			stack;
			return ECMD_FAILED;
		}
	}

	log_print("Volume group \"%s\" successfully backed up.", vg_name);
	return 0;
}

int vgcfgbackup(int argc, char **argv)
{
	return process_each_vg(argc, argv, LCK_READ, &vg_backup_single);
}

