/*
 * Copyright (C) 2001  Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "tools.h"

static int vgscan_single(struct cmd_context *cmd, const char *vg_name);

int vgscan(struct cmd_context *cmd, int argc, char **argv)
{
	if (argc) {
		log_error("Too many parameters on command line");
		return EINVALID_CMD_LINE;
	}

	log_verbose("Wiping cache of LVM-capable devices");
	persistent_filter_wipe(cmd->filter);

	log_verbose("Wiping internal cache of PVs in VGs");
	vgcache_destroy();

	log_print("Reading all physical volumes.  This may take a while...");

	return process_each_vg(cmd, argc, argv, LCK_READ, &vgscan_single);
}

static int vgscan_single(struct cmd_context *cmd, const char *vg_name)
{
	struct volume_group *vg;

	log_verbose("Checking for volume group \"%s\"", vg_name);

	if (!(vg = cmd->fid->ops->vg_read(cmd->fid, vg_name))) {
		log_error("Volume group \"%s\" not found", vg_name);
		return ECMD_FAILED;
	}

	log_print("Found %svolume group \"%s\"", 
		  (vg->status & EXPORTED_VG) ? "exported " : "",
		  vg_name);

	return 0;
}
