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

static int vgimport_single(struct cmd_context *cmd, const char *vg_name);

int vgimport(struct cmd_context *cmd, int argc, char **argv)
{
        if (!argc && !arg_count(cmd,all_ARG)) {
                log_error("Please supply volume groups or use -a for all.");
                return ECMD_FAILED;
        }

        if (argc && arg_count(cmd,all_ARG)) {
                log_error("No arguments permitted when using -a for all.");
                return ECMD_FAILED;
        }

	return process_each_vg(cmd, argc, argv, LCK_WRITE, &vgimport_single);
}

static int vgimport_single(struct cmd_context *cmd, const char *vg_name)
{
	struct volume_group *vg;

	if (!(vg = cmd->fid->ops->vg_read(cmd->fid, vg_name))) {
		log_error("Unable to find exported volume group \"%s\"",
			  vg_name);
		goto error;
	}

	if (!(vg->status & EXPORTED_VG)) {
		log_error("Volume group \"%s\" is not exported", vg_name);
		goto error;
	}

	if (vg->status & PARTIAL_VG) {
		log_error("Volume group \"%s\" is partially missing", vg_name);
		goto error;
	}

	if (!archive(vg))
		goto error;

	vg->status &= ~EXPORTED_VG;

	if (!cmd->fid->ops->vg_write(cmd->fid,vg))
		goto error;

	backup(vg);

	log_print("Volume group \"%s\" successfully imported", vg->name);
	
	return 0;

      error:
	return ECMD_FAILED;
}
