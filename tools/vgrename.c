/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

static struct volume_group *_get_old_vg_for_rename(struct cmd_context *cmd,
						   const char *vg_name_old,
						   const char *vgid)
{
	struct volume_group *vg;

	/* FIXME we used to print an error about EXPORTED, but proceeded
	   nevertheless. */
	vg = vg_read_for_update(cmd, vg_name_old, vgid, READ_ALLOW_EXPORTED);
	if (vg_read_error(vg)) {
		release_vg(vg);
		return_NULL;
	}

	return vg;
}

static int _lock_new_vg_for_rename(struct cmd_context *cmd,
				   const char *vg_name_new)
{
	int rc;

	log_verbose("Checking for new volume group \"%s\"", vg_name_new);

	rc = vg_lock_newname(cmd, vg_name_new);

	if (rc == FAILED_LOCKING) {
		log_error("Can't get lock for %s", vg_name_new);
		return 0;
	}

	if (rc == FAILED_EXIST) {
		log_error("New volume group \"%s\" already exists",
			  vg_name_new);
		return 0;
	}
	return 1;
}

static int vg_rename_path(struct cmd_context *cmd, const char *old_vg_path,
			  const char *new_vg_path)
{
	char *dev_dir;
	struct id id;
	int match = 0;
	int found_id = 0;
	struct dm_list *vgids;
	struct str_list *sl;
	const char *vg_name_new;
	const char *vgid = NULL, *vg_name, *vg_name_old;
	char old_path[NAME_LEN], new_path[NAME_LEN];
	struct volume_group *vg = NULL;
	int lock_vg_old_first = 1;

	vg_name_old = skip_dev_dir(cmd, old_vg_path, NULL);
	vg_name_new = skip_dev_dir(cmd, new_vg_path, NULL);

	dev_dir = cmd->dev_dir;

	if (!validate_vg_rename_params(cmd, vg_name_old, vg_name_new))
		return_0;

	log_verbose("Checking for existing volume group \"%s\"", vg_name_old);

	/* populate lvmcache */
	if (!lvmetad_vg_list_to_lvmcache(cmd))
		stack;

	/* Avoid duplicates */
	if (!(vgids = get_vgids(cmd, 0)) || dm_list_empty(vgids)) {
		log_error("No complete volume groups found");
		return 0;
	}

	dm_list_iterate_items(sl, vgids) {
		vgid = sl->str;
		if (!vgid || !(vg_name = lvmcache_vgname_from_vgid(NULL, vgid)))
			continue;
		if (!strcmp(vg_name, vg_name_old)) {
			if (match) {
				log_error("Found more than one VG called %s. "
					  "Please supply VG uuid.", vg_name_old);
				return 0;
			}
			match = 1;
		}
	}

	log_suppress(2);
	found_id = id_read_format(&id, vg_name_old);
	log_suppress(0);
	if (found_id && (vg_name = lvmcache_vgname_from_vgid(cmd->mem, (char *)id.uuid))) {
		vg_name_old = vg_name;
		vgid = (char *)id.uuid;
	} else
		vgid = NULL;

	if (strcmp(vg_name_new, vg_name_old) < 0)
		lock_vg_old_first = 0;

	if (lock_vg_old_first) {
		vg = _get_old_vg_for_rename(cmd, vg_name_old, vgid);
		if (!vg)
			return_0;

		if (!_lock_new_vg_for_rename(cmd, vg_name_new)) {
			unlock_and_release_vg(cmd, vg, vg_name_old);
			return_0;
		}
	} else {
		if (!_lock_new_vg_for_rename(cmd, vg_name_new))
			return_0;

		vg = _get_old_vg_for_rename(cmd, vg_name_old, vgid);
		if (!vg) {
			unlock_vg(cmd, vg_name_new);
			return_0;
		}
	}

	if (!archive(vg))
		goto error;

	/* Remove references based on old name */
	if (!drop_cached_metadata(vg))
		stack;

	/* Change the volume group name */
	vg_rename(cmd, vg, vg_name_new);

	/* store it on disks */
	log_verbose("Writing out updated volume group");
	if (!vg_write(vg) || !vg_commit(vg)) {
		goto error;
	}

	sprintf(old_path, "%s%s", dev_dir, vg_name_old);
	sprintf(new_path, "%s%s", dev_dir, vg_name_new);

	if (activation() && dir_exists(old_path)) {
		log_verbose("Renaming \"%s\" to \"%s\"", old_path, new_path);

		if (test_mode())
			log_verbose("Test mode: Skipping rename.");

		else if (lvs_in_vg_activated(vg)) {
			if (!vg_refresh_visible(cmd, vg)) {
				log_error("Renaming \"%s\" to \"%s\" failed", 
					old_path, new_path);
				goto error;
			}
		}
	}

	if (!backup(vg))
		stack;
	if (!backup_remove(cmd, vg_name_old))
		stack;

	unlock_vg(cmd, vg_name_new);
	unlock_and_release_vg(cmd, vg, vg_name_old);

	log_print_unless_silent("Volume group \"%s\" successfully renamed to \"%s\"",
				vg_name_old, vg_name_new);

	/* FIXME lvmcache corruption - vginfo duplicated instead of renamed */
	if (cmd->filter->wipe)
		cmd->filter->wipe(cmd->filter);
	lvmcache_destroy(cmd, 1);

	return 1;

      error:
	if (lock_vg_old_first) {
		unlock_vg(cmd, vg_name_new);
		unlock_and_release_vg(cmd, vg, vg_name_old);
	} else {
		unlock_and_release_vg(cmd, vg, vg_name_old);
		unlock_vg(cmd, vg_name_new);
	}
	return 0;
}

int vgrename(struct cmd_context *cmd, int argc, char **argv)
{
	if (argc != 2) {
		log_error("Old and new volume group names need specifying");
		return EINVALID_CMD_LINE;
	}

	if (!vg_rename_path(cmd, argv[0], argv[1]))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

