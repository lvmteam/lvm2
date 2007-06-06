/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved. 
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

int vgrename(struct cmd_context *cmd, int argc, char **argv)
{
	char *dev_dir;
	unsigned length;
	struct id id;
	int consistent = 1;
	int match = 0;
	int found_id = 0;
	struct list *vgids;
	struct str_list *sl;
	char *vg_name_new;
	const char *vgid = NULL, *vg_name, *vg_name_old;
	char old_path[NAME_LEN], new_path[NAME_LEN];
	struct volume_group *vg_old, *vg_new;

	if (argc != 2) {
		log_error("Old and new volume group names need specifying");
		return EINVALID_CMD_LINE;
	}

	vg_name_old = skip_dev_dir(cmd, argv[0], NULL);
	vg_name_new = skip_dev_dir(cmd, argv[1], NULL);

	dev_dir = cmd->dev_dir;
	length = strlen(dev_dir);

	/* Check sanity of new name */
	if (strlen(vg_name_new) > NAME_LEN - length - 2) {
		log_error("New volume group path exceeds maximum length "
			  "of %d!", NAME_LEN - length - 2);
		return ECMD_FAILED;
	}

	if (!validate_vg_name(cmd, vg_name_new)) {
		log_error("New volume group name \"%s\" is invalid",
			  vg_name_new);
		return ECMD_FAILED;
	}

	if (!strcmp(vg_name_old, vg_name_new)) {
		log_error("Old and new volume group names must differ");
		return ECMD_FAILED;
	}

	log_verbose("Checking for existing volume group \"%s\"", vg_name_old);

	/* Avoid duplicates */
	if (!(vgids = get_vgids(cmd, 0)) || list_empty(vgids)) {
		log_error("No complete volume groups found");
		return ECMD_FAILED;
	}

	list_iterate_items(sl, vgids) {
		vgid = sl->str;
		if (!vgid || !(vg_name = vgname_from_vgid(NULL, vgid)) || !*vg_name)
			continue;
		if (!strcmp(vg_name, vg_name_old)) {
			if (match) {
				log_error("Found more than one VG called %s. "
					  "Please supply VG uuid.", vg_name_old);
				return ECMD_FAILED;
			}
			match = 1;
		}
	}

	log_suppress(2);
	found_id = id_read_format(&id, vg_name_old);
	log_suppress(0);
	if (found_id && (vg_name = vgname_from_vgid(cmd->mem, (char *)id.uuid))) {
		vg_name_old = vg_name;
		vgid = (char *)id.uuid;
	} else
		vgid = NULL;

	if (!lock_vol(cmd, vg_name_old, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vg_name_old);
		return ECMD_FAILED;
	}

	if (!(vg_old = vg_read(cmd, vg_name_old, vgid, &consistent)) || !consistent) {
		log_error("Volume group %s %s%s%snot found.", vg_name_old,
		vgid ? "(" : "", vgid ? vgid : "", vgid ? ") " : "");
		unlock_vg(cmd, vg_name_old);
		return ECMD_FAILED;
	}

	if (!vg_check_status(vg_old, CLUSTERED | LVM_WRITE)) {
		unlock_vg(cmd, vg_name_old);
		return ECMD_FAILED;
	}

	/* Don't return failure for EXPORTED_VG */
	vg_check_status(vg_old, EXPORTED_VG);

	if (lvs_in_vg_activated_by_uuid_only(vg_old)) {
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
	if ((vg_new = vg_read(cmd, vg_name_new, NULL, &consistent))) {
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

	/* FIXME lvmcache corruption - vginfo duplicated instead of renamed */
        persistent_filter_wipe(cmd->filter);
        lvmcache_destroy();

	return ECMD_PROCESSED;

      error:
	unlock_vg(cmd, vg_name_new);
	unlock_vg(cmd, vg_name_old);
	return ECMD_FAILED;
}

