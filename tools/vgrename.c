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

char *lv_change_vgname(char *vg_name, char *lv_name);

int vgrename(int argc, char **argv)
{
	int l = 0;
	int length = 0;
	int p = 0;

	int ret = 0;
	char *lv_name_ptr;
	char *vg_name_old;
	char *vg_name_new;
	char vg_name_old_buf[NAME_LEN] = { 0, };
	char vg_name_new_buf[NAME_LEN] = { 0, };
	char *prefix;

	struct io_space *ios;
	struct volume_group *vg_old, *vg_new;

	if (argc != 2) {
		log_error("command line too short");
		return EINVALID_CMD_LINE;
	}

	ios = active_ios();

	prefix = lvm_dir_prefix();
	length = strlen(prefix);

	vg_name_old = argv[0];

	if (strlen(vg_name_new = argv[1]) > NAME_LEN - length - 2) {
		log_error("New logical volume path exceeds maximum length "
			  "of %d!", NAME_LEN - length - 2);
		return ECMD_FAILED;
	}

	if (vg_check_name(vg_name_new) < 0) {
		return EINVALID_CMD_LINE;
	}

	/* FIXME Handle prefix-related logic internally within ios functions? */
	if (strncmp(vg_name_old, prefix, length) != 0) {
		sprintf(vg_name_old_buf, "%s%s", prefix, vg_name_old);
		vg_name_old = vg_name_old_buf;
	}
	if (strncmp(vg_name_new, prefix, length) != 0) {
		sprintf(vg_name_new_buf, "%s%s", prefix, vg_name_new);
		vg_name_new = vg_name_new_buf;
	}

	if (strcmp(vg_name_old, vg_name_new) == 0) {
		log_error("volume group names must be different");
		return ECMD_FAILED;
	}

	log_verbose("Checking existing volume group %s", vg_name_old);
	if (!(vg_old = ios->vg_read(ios, vg_name_old))) {
		log_error("volume group %s doesn't exist", vg_name_old);
		return ECMD_FAILED;
	}
	if (vg_old->status & ACTIVE) {
		log_error("Volume group %s still active", vg_name_old);
	}

	log_verbose("Checking new volume group %s", vg_name_new);
	if ((vg_new = ios->vg_read(ios, vg_name_new))) {
		log_error("New volume group %s already exists", vg_name_new);
		return ECMD_FAILED;
	}

	/* change the volume name in all structures */
	strcpy(vg_old->name, vg_name_new);

	/* FIXME: Are these necessary? Or can vg_write fix these implicitly? */
	for (p = 0; p < vg_old->pv_count; p++)
		if (vg_old->pv[p])
			strcpy(vg_old->pv[p]->vg_name, vg_name_new);

	for (l = 0; l < vg_old->lv_count; l++) {
		if (vg_old->lv[l] &&
		    !(lv_name_ptr =
		      lv_change_vgname(vg_name_new, vg_old->lv[l]->name))) {
			log_error("A new logical volume path exceeds "
				  "maximum of %d!", NAME_LEN - 2);
			return ECMD_FAILED;
		}
		strcpy(vg_old->lv[l]->name, lv_name_ptr);
	}

	if (vg_remove_dir_and_group_and_nodes(vg_name_old) < 0) {
		log_error("removing volume group nodes and directory of \"%s\"",
			  vg_name_old);
		return ECMD_FAILED;
	}

	/* store it on disks */
	log_verbose("updating volume group name");
	if (ios->vg_write(ios, vg_old)) {
		return ECMD_FAILED;
	}

	log_verbose("creating volume group directory %s%s", prefix,
		    vg_name_new);
	if (vg_create_dir_and_group_and_nodes(vg_old)) {
		return ECMD_FAILED;
	}

	if ((ret = do_autobackup(vg_name_new, vg_old)))
		return ECMD_FAILED;

	log_print("Volume group %s successfully renamed to %s",
		  vg_name_old, vg_name_new);

	return 0;

/* FIXME: Deallocations */
}

/* FIXME: Move this out */

char *lv_change_vgname(char *vg_name, char *lv_name)
{
	char *lv_name_ptr = NULL;
	static char lv_name_buf[NAME_LEN] = { 0, };

	/* check if lv_name includes a path */
	if ((lv_name_ptr = strrchr(lv_name, '/'))) {
		lv_name_ptr++;
		sprintf(lv_name_buf, "%s%s/%s%c", lvm_dir_prefix(), vg_name,
			lv_name_ptr, 0);
	} else
		strncpy(lv_name_buf, lv_name, NAME_LEN - 1);
	return lv_name_buf;
}
