/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "fs.h"
#include "log.h"
#include "names.h"

#include <libdevmapper.h>


/*
 * Lazy programmer: I'm just going to always try
 * and create/remove the vg directory, and not say
 * anything if it fails.
 */
static int _mk_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	if (!build_vg_path(vg_path, sizeof(vg_path),
			   vg->cmd->dev_dir, vg->name)) {
		log_error("Couldn't construct name of volume group directory.");
		return 0;
	}

	log_very_verbose("Creating directory %s", vg_path);
	mkdir(vg_path, 0555);

	return 1;
}

static int _rm_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	if (!build_vg_path(vg_path, sizeof(vg_path),
			   vg->cmd->dev_dir, vg->name)) {
		log_error("Couldn't construct name of volume group dir for %s",
			  vg->name);
		return 0;
	}

	log_very_verbose("Removing directory %s", vg_path);
	rmdir(vg_path);

	return 1;
}

static int _mk_link(struct logical_volume *lv)
{
	char lv_path[PATH_MAX], link_path[PATH_MAX];
	struct stat buf;

	if (!build_dm_path(lv_path, sizeof(lv_path), lv->vg->name, lv->name)) {
		log_error("Couldn't create destination pathname for "
			  "logical volume link for %s", lv->name);
		return 0;
	}

	if (!build_lv_link_path(link_path, sizeof(link_path),
				lv->vg->cmd->dev_dir,
				lv->vg->name, lv->name)) {
		log_error("Couldn't create source pathname for "
			  "logical volume link %s", lv->name);
		return 0;
	}

	if (!lstat(link_path, &buf)) {
		if (!S_ISLNK(buf.st_mode)) {
			log_error("Symbolic link %s not created: file exists",
				  link_path);
			return 0;
		}
		if (unlink(link_path) < 0) {
			log_sys_error("unlink", link_path);
			return 0;
		}
	}

	log_very_verbose("Linking %s to %s", link_path, lv_path);
	if (symlink(lv_path, link_path) < 0) {
		log_sys_error("symlink", link_path);
		return 0;
	}

	return 1;
}

static int _rm_link(struct logical_volume *lv, const char *lv_name)
{
	struct stat buf;
	char link_path[PATH_MAX];

	if (!lv_name)
		lv_name = lv->name;

	if (!build_lv_link_path(link_path, sizeof(link_path),
				lv->vg->cmd->dev_dir,
				lv->vg->name, lv->name)) {
		log_error("Couldn't determine link pathname.");
		return 0;
	}

	log_very_verbose("Removing link %s", link_path);
	if (lstat(link_path, &buf) || !S_ISLNK(buf.st_mode)) {
			log_error("%s not symbolic link - not removing",
				  link_path);
			return 0;
	}
	if (unlink(link_path) < 0) {
		log_sys_error("unlink", link_path);
		return 0;
	}

	return 1;
}

int fs_add_lv(struct logical_volume *lv)
{
	if (!_mk_dir(lv->vg) ||
	    !_mk_link(lv)) {
		stack;
		return 0;
	}

	return 1;
}

int fs_del_lv(struct logical_volume *lv)
{
	if (!_rm_link(lv, NULL) ||
	    !_rm_dir(lv->vg)) {
		stack;
		return 0;
	}

	return 1;
}

int fs_rename_lv(const char *old_name, struct logical_volume *lv)
{
	if (!_rm_link(lv, old_name))
		stack;

	if (!_mk_link(lv))
		stack;

	return 1;
}
