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
#include "lvm-types.h"

int lvrename(struct cmd_context *cmd, int argc, char **argv)
{
	size_t maxlen;
	char *lv_name_old, *lv_name_new;
	const char *vg_name, *vg_name_new, *vg_name_old;
	char *st;
	int consistent = 1;

	struct volume_group *vg;
	struct logical_volume *lv;
	struct lv_list *lvl;

	if (argc == 3) {
		vg_name = argv[0];
		if (!strncmp(vg_name, cmd->dev_dir, strlen(cmd->dev_dir)))
			vg_name += strlen(cmd->dev_dir);
		lv_name_old = argv[1];
		lv_name_new = argv[2];
		if (strchr(lv_name_old, '/') &&
		    (vg_name_old = extract_vgname(cmd, lv_name_old)) &&
		    strcmp(vg_name_old, vg_name)) {
			log_error("Please use a single volume group name "
				  "(\"%s\" or \"%s\")", vg_name, vg_name_old);
			return EINVALID_CMD_LINE;
		}
	} else if (argc == 2) {
		lv_name_old = argv[0];
		lv_name_new = argv[1];
		vg_name = extract_vgname(cmd, lv_name_old);
	} else {
		log_error("Old and new logical volume names required");
		return EINVALID_CMD_LINE;
	}

	if (!validate_name(vg_name)) {
		log_error("Please provide a valid volume group name");
		return EINVALID_CMD_LINE;
	}

	if (strchr(lv_name_new, '/') &&
	    (vg_name_new = extract_vgname(cmd, lv_name_new)) &&
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
			  "of %" PRIsize_t "!", maxlen);
		return ECMD_FAILED;
	}

	if (!*lv_name_new) {
		log_error("New logical volume name may not be blank");
		return ECMD_FAILED;
	}

	/* FIXME Remove this restriction eventually */
	if (!strncmp(lv_name_new, "snapshot", 8)) {
		log_error("Names starting \"snapshot\" are reserved. "
			  "Please choose a different LV name.");
		return ECMD_FAILED;
	}

	if (!validate_name(lv_name_new)) {
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

	if (!(vg = vg_read(cmd, vg_name, &consistent))) {
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

	if (!lock_vol(cmd, lv->lvid.s, LCK_LV_SUSPEND | LCK_HOLD |
		      LCK_NONBLOCK))
		goto error;

	if (!(lv->name = pool_strdup(cmd->mem, lv_name_new))) {
		log_error("Failed to allocate space for new name");
		goto lverror;
	}

	log_verbose("Writing out updated volume group");
	if (!vg_write(vg))
		goto lverror;

	unlock_lv(cmd, lv->lvid.s);

	backup(lv->vg);

	unlock_vg(cmd, vg_name);

	log_print("Renamed \"%s\" to \"%s\" in volume group \"%s\"",
		  lv_name_old, lv_name_new, vg_name);

	return 0;

      lverror:
	unlock_lv(cmd, lv->lvid.s);

      error:
	unlock_vg(cmd, vg_name);
	return ECMD_FAILED;
}
