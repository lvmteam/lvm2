/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

static int vg_backup_single(const char *vg_name)
{
	struct volume_group *vg;

	log_verbose("Checking for volume group %s", vg_name);
	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Volume group %s not found", vg_name);
		return ECMD_FAILED;
	}

	log_print("Found %sactive volume group %s",
		  (vg->status & ACTIVE) ? "" : "in", vg_name);

	if (!autobackup(vg)) {
		stack;
		return ECMD_FAILED;
	}

	log_print("Volume group %s successfully backed up.", vg_name);
	return 0;
}

int vgcfgbackup(int argc, char **argv)
{
	return process_each_vg(argc, argv, &vg_backup_single);
}

