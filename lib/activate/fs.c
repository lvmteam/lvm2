/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "fs.h"
#include "log.h"
#include "toolcontext.h"
#include "lvm-string.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <libdevmapper.h>
#include <dirent.h>

/*
 * Lazy programmer: I'm just going to always try
 * and create/remove the vg directory, and not say
 * anything if it fails.
 */
static int _mk_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	if (lvm_snprintf(vg_path, sizeof(vg_path), "%s%s",
			 vg->cmd->dev_dir, vg->name) == -1) {
		log_error("Couldn't construct name of volume "
			  "group directory.");
		return 0;
	}

	log_very_verbose("Creating directory %s", vg_path);
	mkdir(vg_path, 0555);

	return 1;
}

static int _is_empty_dir(const char *dir)
{
	int i, count, r = 1;
	struct dirent **dirent;
	const char *name;

	count = scandir(dir, &dirent, NULL, alphasort);
	if (!count)
		return 1;

	if (count < 0) {
		log_err("Couldn't scan directory '%s'.", dir);
		return 0;
	}

	/*
	 * Scan the devices.
	 */
	for (i = 0; i < count; i++) {
		name = dirent[i]->d_name;

		/*
		 * Ignore dot files.
		 */
		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;

		r = 0;
		break;
	}

	/*
	 * Free the directory entries.
	 */
	for (i = 0; i < count; i++)
		free(dirent[i]);
	free(dirent);

	return r;
}

static int _rm_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	if (lvm_snprintf(vg_path, sizeof(vg_path), "%s%s",
			 vg->cmd->dev_dir, vg->name) == -1) {
		log_error("Couldn't construct name of volume "
			  "group directory.");
		return 0;
	}

	log_very_verbose("Removing directory %s", vg_path);

	if (_is_empty_dir(vg_path))
		rmdir(vg_path);

	return 1;
}

static int _mk_link(struct logical_volume *lv, const char *dev)
{
	char lv_path[PATH_MAX], link_path[PATH_MAX];
	struct stat buf;

	if (lvm_snprintf(lv_path, sizeof(lv_path), "%s%s/%s",
			 lv->vg->cmd->dev_dir, lv->vg->name, lv->name) == -1) {
		log_error("Couldn't create source pathname for "
			  "logical volume link %s", lv->name);
		return 0;
	}

	if (lvm_snprintf(link_path, sizeof(link_path), "%s/%s",
			 dm_dir(), dev) == -1) {
		log_error("Couldn't create destination pathname for "
			  "logical volume link for %s", lv->name);
		return 0;
	}

	if (!lstat(lv_path, &buf)) {
		if (!S_ISLNK(buf.st_mode)) {
			log_error("Symbolic link %s not created: file exists",
				  link_path);
			return 0;
		}

		if (unlink(lv_path) < 0) {
			log_sys_error("unlink", lv_path);
			return 0;
		}
	}

	log_very_verbose("Linking %s -> %s", lv_path, link_path);
	if (symlink(link_path, lv_path) < 0) {
		log_sys_error("symlink", lv_path);
		return 0;
	}

	return 1;
}

static int _rm_link(struct logical_volume *lv, const char *lv_name)
{
	struct stat buf;
	char lv_path[PATH_MAX];

	if (lvm_snprintf(lv_path, sizeof(lv_path), "%s%s/%s",
			 lv->vg->cmd->dev_dir, lv->vg->name, lv_name) == -1) {
		log_error("Couldn't determine link pathname.");
		return 0;
	}

	log_very_verbose("Removing link %s", lv_path);
	if (lstat(lv_path, &buf) || !S_ISLNK(buf.st_mode)) {
		log_error("%s not symbolic link - not removing", lv_path);
		return 0;
	}

	if (unlink(lv_path) < 0) {
		log_sys_error("unlink", lv_path);
		return 0;
	}

	return 1;
}

int fs_add_lv(struct logical_volume *lv, const char *dev)
{
	if (!_mk_dir(lv->vg) ||
	    !_mk_link(lv, dev)) {
		stack;
		return 0;
	}

	return 1;
}

int fs_del_lv(struct logical_volume *lv)
{
	if (!_rm_link(lv, lv->name) ||
	    !_rm_dir(lv->vg)) {
		stack;
		return 0;
	}

	return 1;
}

int fs_rename_lv(struct logical_volume *lv,
		 const char *dev, const char *old_name)
{
	if (!_rm_link(lv, old_name))
		stack;

	if (!_mk_link(lv, dev))
		stack;

	return 1;
}
