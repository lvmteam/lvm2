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

int vgrename(int argc, char **argv)
{
	char *dev_dir;
	int length;

	char *vg_name_old, *vg_name_new;

	char old_path[NAME_LEN], new_path[NAME_LEN];

	struct volume_group *vg_old, *vg_new;
	struct list *pvh;

	if (argc != 2) {
		log_error("Old and new volume group names need specifying");
		return EINVALID_CMD_LINE;
	}

	vg_name_old = argv[0];
	vg_name_new = argv[1];

	dev_dir = fid->cmd->dev_dir;
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

	if (!is_valid_chars(vg_name_new)) {
		log_error("New volume group name %s has invalid characters",
			  vg_name_new);
		return ECMD_FAILED;
	}

	if (!strcmp(vg_name_old, vg_name_new)) {
		log_error("Old and new volume group names must differ");
		return ECMD_FAILED;
	}

	log_verbose("Checking for existing volume group %s", vg_name_old);
	if (!(vg_old = fid->ops->vg_read(fid, vg_name_old))) {
		log_error("Volume group %s doesn't exist", vg_name_old);
		return ECMD_FAILED;
	}

	if (vg_old->status & ACTIVE) {
		log_error("Volume group %s still active", vg_name_old);
		if (!force_ARG) {
			log_error("Use -f to force the rename");
			return ECMD_FAILED;
		}
	}

	log_verbose("Checking for new volume group %s", vg_name_new);
	if ((vg_new = fid->ops->vg_read(fid, vg_name_new))) {
		log_error("New volume group %s already exists", vg_name_new);
		return ECMD_FAILED;
	}

	/* Change the volume group name */
	strcpy(vg_old->name, vg_name_new);

	/* FIXME Should vg_write fix these implicitly? It has to check them. */
	list_iterate(pvh, &vg_old->pvs) {
		strcpy(list_item(pvh, struct pv_list)->pv.vg_name,
		       vg_name_new);
	}

/********** FIXME: Check within vg_write now
			log_error("A new logical volume path exceeds "
				  "maximum of %d!", NAME_LEN - 2);
			return ECMD_FAILED;
*************/

	sprintf(old_path, "%s%s", dev_dir, vg_name_old);
	sprintf(new_path, "%s%s", dev_dir, vg_name_new);

	log_verbose("Renaming %s to %s", old_path, new_path);
	if (rename(old_path, new_path)) {
		log_error("Renaming %s to %s failed: %s",
			  old_path, new_path, strerror(errno));
		return ECMD_FAILED;
	}

	/* store it on disks */
	log_verbose("Writing out updated volume group");
	if (!(fid->ops->vg_write(fid, vg_old))) {
		return ECMD_FAILED;
	}

/******* FIXME Any LV things to update? */

	backup(vg_old);

	log_print("Volume group %s successfully renamed to %s",
		  vg_name_old, vg_name_new);

	/* FIXME: Deallocations */

	return 0;
}

/* FIXME: Moved into vg_write now */
/*******************
char *lv_change_vgname(char *vg_name, char *lv_name)
{
	char *lv_name_ptr = NULL;
	static char lv_name_buf[NAME_LEN] = { 0, };

	** check if lv_name includes a path 
	if ((lv_name_ptr = strrchr(lv_name, '/'))) {
	    lv_name_ptr++;
	    sprintf(lv_name_buf, "%s%s/%s%c", fid->cmd->dev_dir, vg_name,
		    lv_name_ptr, 0);} 
	else
	    strncpy(lv_name_buf, lv_name, NAME_LEN - 1); return lv_name_buf;}

**********************/
