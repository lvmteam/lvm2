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

int lvrename(struct cmd_context *cmd, int argc, char **argv)
{
	int maxlen;
	int active;
	char *lv_name_old, *lv_name_new;
	char *vg_name, *vg_name_new;
	char *st;

	struct volume_group *vg;
	struct logical_volume *lv;
	struct lv_list *lvl;

	if (argc != 2) {
		log_error("Old and new logical volume required");
		return EINVALID_CMD_LINE;
	}

	lv_name_old = argv[0];
	lv_name_new = argv[1];

	if (!(vg_name = extract_vgname(cmd->fid, lv_name_old))) {
		log_error("Please provide a volume group name");
		return EINVALID_CMD_LINE;
	}

	if (strchr(lv_name_new, '/') &&
	    (vg_name_new = extract_vgname(cmd->fid, lv_name_new)) &&
	    strcmp(vg_name, vg_name_new)) {
		log_error("Logical volume names must "
			  "have the same volume group (\"%s\" or \"%s\")",
			  vg_name, vg_name_new);
		return EINVALID_CMD_LINE;
	}

	if ((st = strrchr(lv_name_old, '/')))
		lv_name_old = st + 1;

	if ((st = strrchr(lv_name_new, '/')))
		lv_name_new = st + 1;

	/* Check sanity of new name */
	maxlen = NAME_LEN - strlen(vg_name) - strlen(cmd->dev_dir) - 3;
	if (strlen(lv_name_new) > maxlen) {
		log_error("New logical volume path exceeds maximum length "
			  "of %d!", maxlen);
		return ECMD_FAILED;
	}

	if (!*lv_name_new) {
		log_error("New logical volume name may not be blank");
		return ECMD_FAILED;
	}

	if (!is_valid_chars(lv_name_new)) {
		log_error
		    ("New logical volume name \"%s\" has invalid characters",
		     lv_name_new);
		return EINVALID_CMD_LINE;
	}

	if (!strcmp(lv_name_old, lv_name_new)) {
		log_error("Old and new logical volume names must differ");
		return EINVALID_CMD_LINE;
	}

	log_verbose("Checking for existing volume group \"%s\"", vg_name);

	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg = cmd->fid->ops->vg_read(cmd->fid, vg_name))) {
		log_error("Volume group \"%s\" doesn't exist", vg_name);
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

	if (find_lv_in_vg(vg, lv_name_new)) {
		log_error("Logical volume \"%s\" already exists in "
			  "volume group \"%s\"", lv_name_new, vg_name);
		goto error;
	}

	if (!(lvl = find_lv_in_vg(vg, lv_name_old))) {
		log_error("Existing logical volume \"%s\" not found in "
			  "volume group \"%s\"", lv_name_old, vg_name);
		goto error;
	}

	lv = lvl->lv;

	if (!archive(lv->vg))
		goto error;

	if ((active = lv_active(lv)) < 0) {
		log_error("Unable to determine status of \"%s\"", lv->name);
		goto error;
	}

	if (active && !lv_suspend(lv)) {
		log_error("Failed to suspend \"%s\"", lv->name);
		goto error;
	}

	if (!(lv->name = pool_strdup(cmd->mem, lv_name_new))) {
		log_error("Failed to allocate space for new name");
		goto error;
	}

	/* store it on disks */
	log_verbose("Writing out updated volume group");
	if (!(cmd->fid->ops->vg_write(cmd->fid, vg))) {
		goto error;
	}

	if (active) {
		lv_rename(lv_name_old, lv);
		lv_reactivate(lv);
	}

	backup(lv->vg);

	lock_vol(cmd, vg_name, LCK_VG_UNLOCK);

	log_print("Renamed \"%s\" to \"%s\" in volume group \"%s\"",
		  lv_name_old, lv_name_new, vg_name);

	return 0;

      error:
	lock_vol(cmd, vg_name, LCK_VG_UNLOCK);
	return ECMD_FAILED;
}
