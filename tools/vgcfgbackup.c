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
	void *context;

	if (!(context = create_text_context(vg->cmd->fmtt, file,
					    vg->cmd->cmd_line)) ||
	    !(tf = vg->cmd->fmtt->ops->create_instance(vg->cmd->fmtt, NULL,
						       context))) {
		log_error("Couldn't create backup object.");
		return 0;
	}

	if (!(r = tf->fmt->ops->vg_write(tf, vg, context)) ||
	    !(r = tf->fmt->ops->vg_commit(tf, vg, context)))
		stack;

	tf->fmt->ops->destroy_instance(tf);
	return r;
}

static int vg_backup_single(struct cmd_context *cmd, const char *vg_name)
{
	struct volume_group *vg;

	log_verbose("Checking for volume group \"%s\"", vg_name);
	if (!(vg = vg_read(cmd, vg_name))) {
		log_error("Volume group \"%s\" not found", vg_name);
		return ECMD_FAILED;
	}

	if (arg_count(cmd, file_ARG)) {
		_backup_to_file(arg_value(cmd, file_ARG), vg);

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

int vgcfgbackup(struct cmd_context *cmd, int argc, char **argv)
{
	if (!driver_is_loaded())
		return ECMD_FAILED;     

	return process_each_vg(cmd, argc, argv, LCK_VG_READ, &vg_backup_single);
}
