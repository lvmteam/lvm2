/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "fs.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "lvm-file.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <libdevmapper.h>

static int _mk_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	if (lvm_snprintf(vg_path, sizeof(vg_path), "%s%s",
			 vg->cmd->dev_dir, vg->name) == -1) {
		log_error("Couldn't construct name of volume "
			  "group directory.");
		return 0;
	}

	if (dir_exists(vg_path))
		return 1;

	log_very_verbose("Creating directory %s", vg_path);
	if (mkdir(vg_path, 0555)) {
		log_sys_error("mkdir", vg_path);
		return 0;
	}

	return 1;
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

	if (is_empty_dir(vg_path))
		rmdir(vg_path);

	return 1;
}

static void _rm_blks(const char *dir)
{
	const char *name;
	char path[PATH_MAX];
	struct dirent *dirent;
	struct stat buf;
	DIR *d;

	if (!(d = opendir(dir))) {
		log_sys_error("opendir", dir);
		return;
	}

	while ((dirent = readdir(d))) {
		name = dirent->d_name;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;

		if (lvm_snprintf(path, sizeof(path), "%s/%s", dir, name) == -1) {
			log_error("Couldn't create path for %s", name);
			continue;
		}

		if (!lstat(path, &buf)) {
			if (!S_ISBLK(buf.st_mode))
				continue;
			log_very_verbose("Removing %s", path);
			if (unlink(path) < 0)
				log_sys_error("unlink", path);
		}
	}
}

static int _mk_link(struct logical_volume *lv, const char *dev)
{
	char lv_path[PATH_MAX], link_path[PATH_MAX], lvm1_group_path[PATH_MAX];
	char vg_path[PATH_MAX];
	struct stat buf;

	if (lvm_snprintf(vg_path, sizeof(vg_path), "%s%s",
			 lv->vg->cmd->dev_dir, lv->vg->name) == -1) {
		log_error("Couldn't create path for volume group dir %s",
			  lv->vg->name);
		return 0;
	}

	if (lvm_snprintf(lv_path, sizeof(lv_path), "%s/%s", vg_path,
			 lv->name) == -1) {
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

	if (lvm_snprintf(lvm1_group_path, sizeof(lvm1_group_path), "%s/group",
			 vg_path) == -1) {
		log_error("Couldn't create pathname for LVM1 group file for %s",
			  lv->vg->name);
		return 0;
	}

	/* To reach this point, the VG must have been locked.
	 * As locking fails if the VG is active under LVM1, it's 
	 * now safe to remove any LVM1 devices we find here
	 * (as well as any existing LVM2 symlink). */
	if (!lstat(lvm1_group_path, &buf)) {
		if (!S_ISCHR(buf.st_mode)) {
			log_error("Non-LVM1 character device found at %s",
				  lvm1_group_path);
		} else {
			_rm_blks(vg_path);

			log_very_verbose("Removing %s", lvm1_group_path);
			if (unlink(lvm1_group_path) < 0)
				log_sys_error("unlink", lvm1_group_path);
		}
	}

	if (!lstat(lv_path, &buf)) {
		if (!S_ISLNK(buf.st_mode) && !S_ISBLK(buf.st_mode)) {
			log_error("Symbolic link %s not created: file exists",
				  link_path);
			return 0;
		}

		log_very_verbose("Removing %s", lv_path);
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

	if (lstat(lv_path, &buf) || !S_ISLNK(buf.st_mode)) {
		log_error("%s not symbolic link - not removing", lv_path);
		return 0;
	}

	log_very_verbose("Removing link %s", lv_path);
	if (unlink(lv_path) < 0) {
		log_sys_error("unlink", lv_path);
		return 0;
	}

	return 1;
}

int fs_add_lv(struct logical_volume *lv, const char *dev)
{
	if (!_mk_dir(lv->vg) || !_mk_link(lv, dev)) {
		stack;
		return 0;
	}

	return 1;
}

int fs_del_lv(struct logical_volume *lv)
{
	if (!_rm_link(lv, lv->name) || !_rm_dir(lv->vg)) {
		stack;
		return 0;
	}

	return 1;
}

/* FIXME Use rename() */
int fs_rename_lv(struct logical_volume *lv,
		 const char *dev, const char *old_name)
{
	if (old_name && !_rm_link(lv, old_name))
		stack;

	if (!_mk_link(lv, dev))
		stack;

	return 1;
}
