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

int vgextend(struct cmd_context *cmd, int argc, char **argv)
{
	char *vg_name;
	struct volume_group *vg = NULL;

	if (!argc) {
		log_error("Please enter volume group name and "
			  "physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1) {
		log_error("Please enter physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	vg_name = argv[0];
	argc--;
	argv++;

	if (!lock_vol(cmd, "", LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return ECMD_FAILED;
	}

	log_verbose("Checking for volume group \"%s\"", vg_name);
	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE | LCK_NONBLOCK)) {
		unlock_vg(cmd, "");
		log_error("Can't get lock for %s", vg_name);
		goto error;
	}

	if (!(vg = vg_read(cmd, vg_name))) {
		log_error("Volume group \"%s\" not found.", vg_name);
		goto error;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg->name);
		goto error;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", vg_name);
		goto error;
	}

	if (!(vg->status & RESIZEABLE_VG)) {
		log_error("Volume group \"%s\" is not resizeable.", vg_name);
		goto error;
	}

/********** FIXME
	log_print("maximum logical volume size is %s",
		  (dummy = lvm_show_size(LVM_LV_SIZE_MAX(vg) / 2, LONG)));
	dbg_free(dummy);
	dummy = NULL;
**********/

	if (!archive(vg))
		goto error;

	/* extend vg */
	if (!vg_extend(vg->fid, vg, argc, argv))
		goto error;

	/* ret > 0 */
	log_verbose("Volume group \"%s\" will be extended by %d new "
		    "physical volumes", vg_name, argc);

	/* store vg on disk(s) */
	if (!vg_write(vg))
		goto error;

	backup(vg);

	unlock_vg(cmd, vg_name);
	unlock_vg(cmd, "");

	log_print("Volume group \"%s\" successfully extended", vg_name);

	return 0;

      error:
	unlock_vg(cmd, vg_name);
	unlock_vg(cmd, "");
	return ECMD_FAILED;
}
