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

int vgrename(struct cmd_context *cmd, int argc, char **argv)
{
	char *dev_dir;
	unsigned int length;
	int consistent = 1;

	char *vg_name_old, *vg_name_new;

	char old_path[NAME_LEN], new_path[NAME_LEN];

	struct volume_group *vg_old, *vg_new;

	if (argc != 2) {
		log_error("Old and new volume group names need specifying");
		return EINVALID_CMD_LINE;
	}

	vg_name_old = argv[0];
	vg_name_new = argv[1];

	dev_dir = cmd->dev_dir;
	length = strlen(dev_dir);

	/* If present, strip dev_dir */
	if (!strncmp(vg_name_old, dev_dir, length))
		vg_name_old += length;
	if (!strncmp(vg_name_new, dev_dir, length))
		vg_name_new += length;

	/* Check sanity of new name */
	if (strlen(vg_name_new) > NAME_LEN - length - 2) {
		log_error("New volume group path exceeds maximum length "
			  "of %d!", NAME_LEN - length - 2);
		return ECMD_FAILED;
	}

	if (!validate_name(vg_name_new)) {
		log_error("New volume group name \"%s\" is invalid",
			  vg_name_new);
		return ECMD_FAILED;
	}

	if (!strcmp(vg_name_old, vg_name_new)) {
		log_error("Old and new volume group names must differ");
		return ECMD_FAILED;
	}

	log_verbose("Checking for existing volume group \"%s\"", vg_name_old);

	if (!lock_vol(cmd, vg_name_old, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vg_name_old);
		return ECMD_FAILED;
	}

	if (!(vg_old = vg_read(cmd, vg_name_old, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" doesn't exist", vg_name_old);
		unlock_vg(cmd, vg_name_old);
		return ECMD_FAILED;
	}

	if (vg_old->status & EXPORTED_VG) {
		unlock_vg(cmd, vg_name_old);
		log_error("Volume group \"%s\" is exported", vg_old->name);
		return ECMD_FAILED;
	}

	if (!(vg_old->status & LVM_WRITE)) {
		unlock_vg(cmd, vg_name_old);
		log_error("Volume group \"%s\" is read-only", vg_old->name);
		return ECMD_FAILED;
	}

	if (lvs_in_vg_activated(vg_old)) {
		unlock_vg(cmd, vg_name_old);
		log_error("Volume group \"%s\" still has active LVs",
			  vg_name_old);
		/* FIXME Remove this restriction */
		return ECMD_FAILED;
	}

	log_verbose("Checking for new volume group \"%s\"", vg_name_new);

	if (!lock_vol(cmd, vg_name_new, LCK_VG_WRITE | LCK_NONBLOCK)) {
		unlock_vg(cmd, vg_name_old);
		log_error("Can't get lock for %s", vg_name_new);
		return ECMD_FAILED;
	}

	consistent = 0;
	if ((vg_new = vg_read(cmd, vg_name_new, &consistent))) {
		log_error("New volume group \"%s\" already exists",
			  vg_name_new);
		goto error;
	}

	if (!archive(vg_old))
		goto error;

	/* Change the volume group name */
	vg_rename(cmd, vg_old, vg_name_new);

	sprintf(old_path, "%s%s", dev_dir, vg_name_old);
	sprintf(new_path, "%s%s", dev_dir, vg_name_new);

	if (dir_exists(old_path)) {
		log_verbose("Renaming \"%s\" to \"%s\"", old_path, new_path);
		if (test_mode())
			log_verbose("Test mode: Skipping rename.");
		else if (rename(old_path, new_path)) {
			log_error("Renaming \"%s\" to \"%s\" failed: %s",
				  old_path, new_path, strerror(errno));
			goto error;
		}
	}

	/* store it on disks */
	log_verbose("Writing out updated volume group");
	if (!vg_write(vg_old) || !vg_commit(vg_old)) {
		goto error;
	}

/******* FIXME Rename any active LVs! *****/

	backup(vg_old);

	unlock_vg(cmd, vg_name_new);
	unlock_vg(cmd, vg_name_old);

	log_print("Volume group \"%s\" successfully renamed to \"%s\"",
		  vg_name_old, vg_name_new);

	return ECMD_PROCESSED;

      error:
	unlock_vg(cmd, vg_name_new);
	unlock_vg(cmd, vg_name_old);
	return ECMD_FAILED;
}

