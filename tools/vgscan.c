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

static int vgscan_single(struct cmd_context *cmd, const char *vg_name,
			 struct volume_group *vg, int consistent, void *handle)
{
	if (!vg) {
		log_error("Volume group \"%s\" not found", vg_name);
		return ECMD_FAILED;
	}

	if (!consistent) {
		unlock_vg(cmd, vg_name);
		log_error("Volume group \"%s\" inconsistent", vg_name);
		/* Don't allow partial switch to this program */
		if (!(vg = recover_vg(cmd, vg_name, LCK_VG_WRITE)))
			return ECMD_FAILED;
	}

	log_print("Found %svolume group \"%s\" using metadata type %s",
		  (vg->status & EXPORTED_VG) ? "exported " : "", vg_name,
		  vg->fid->fmt->name);

	return ECMD_PROCESSED;
}

int vgscan(struct cmd_context *cmd, int argc, char **argv)
{
	if (argc) {
		log_error("Too many parameters on command line");
		return EINVALID_CMD_LINE;
	}

	log_verbose("Wiping cache of LVM-capable devices");
	persistent_filter_wipe(cmd->filter);

	log_verbose("Wiping internal cache");
	lvmcache_destroy();

	log_print("Reading all physical volumes.  This may take a while...");

	return process_each_vg(cmd, argc, argv, LCK_VG_READ, 1, NULL,
			       &vgscan_single);
}
