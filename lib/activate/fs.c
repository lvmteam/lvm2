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

#include <devmapper/libdevmapper.h>

/*
 * FIXME: copied straight from LVM1.
 *
 * This should run through /proc/mounts once only,
 * storing devfs mount points in a hash table.
 */
static int _check_devfs(const char *dev_prefix)
{
	int r = 0, len;
	char dir[PATH_MAX], line[512];
	char type[32];
	FILE *mounts = NULL;

	if (!(mounts = fopen("/proc/mounts", "r")))
		goto out;

	/* trim the trailing slash off the dir prefix, yuck */
	len = strlen(dev_prefix) - 1;
	while(len && dev_prefix[len] == '/')
		len--;

	while (!feof(mounts)) {
		fgets(line, sizeof(line) - 1, mounts);
		if (sscanf(line, "%*s %s %s %*s", dir, type) != 2)
			continue;

		if (!strcmp(type, "devfs") && !strncmp(dir, dev_prefix, len)) {
			r = 1;
			break;
		}
	}
	fclose(mounts);

 out:
	return r;
}

void _build_lv_path(char *buffer, size_t len, struct logical_volume *lv)
{
	snprintf(buffer, len, "%s%s/%s_%s",
		 lv->vg->cmd->dev_dir, dm_dir(), lv->vg->name, lv->name);
}

void _build_vg_path(char *buffer, size_t len, struct volume_group *vg)
{
	snprintf(buffer, len, "%s/%s", vg->cmd->dev_dir, vg->name);
}

void _build_link_path(char *buffer, size_t len, struct logical_volume *lv)
{
	snprintf(buffer, len, "%s/%s/%s", lv->vg->cmd->dev_dir,
		 lv->vg->name, lv->name);
}

static int _mk_node(struct logical_volume *lv)
{
	char lv_path[PATH_MAX];
	char dm_path[PATH_MAX];
	dev_t dev;
	const char *dev_dir = lv->vg->cmd->dev_dir;

	if (_check_devfs(dev_dir))
		return 1;

	snprintf(dm_path, PATH_MAX, "%s%s", dev_dir, dm_dir());
	if (mkdir(dm_path, 0555) && errno != EEXIST) {
		log_sys_error("mkdir", dm_path);
		return 0;
	}

	_build_lv_path(lv_path, sizeof(lv_path), lv);

	if (mknod(lv_path, S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP, dev) < 0) {
		log_sys_error("mknod", lv_path);
		return 0;
	}

	return 1;
}

static int _rm_node(struct logical_volume *lv)
{
	char lv_path[PATH_MAX];
	const char *dev_dir = lv->vg->cmd->dev_dir;

	if (_check_devfs(dev_dir))
		return 1;

	_build_lv_path(lv_path, sizeof(lv_path), lv);

	if (unlink(lv_path) < 0) {
		log_sys_error("unlink", lv_path);
		return 0;
	}

	return 1;
}

/*
 * Lazy programmer: I'm just going to always try
 * and create/remove the vg directory, and not say
 * anything if it fails.
 */
static int _mk_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	_build_vg_path(vg_path, sizeof(vg_path), vg);
	mkdir(vg_path, 0555);
	return 1;
}

static int _rm_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	_build_vg_path(vg_path, sizeof(vg_path), vg);
	rmdir(vg_path);
	return 1;
}

static int _mk_link(struct logical_volume *lv)
{
	char lv_path[PATH_MAX], link_path[PATH_MAX];

	_build_lv_path(lv_path, sizeof(lv_path), lv);
	_build_link_path(link_path, sizeof(link_path), lv);

	if (symlink(lv_path, link_path) < 0) {
		log_sys_error("symlink", link_path);
		return 0;
	}

	return 1;
}

static int _rm_link(struct logical_volume *lv)
{
	char link_path[PATH_MAX];

	_build_link_path(link_path, sizeof(link_path), lv);

	if (unlink(link_path) < 0) {
		log_sys_error("unlink", link_path);
		return 0;
	}

	return 1;
}

int fs_add_lv(struct logical_volume *lv)
{
	if (!_mk_node(lv)) {
		stack;
		return 0;
	}

	if (!_mk_dir(lv->vg)) {
		stack;
		return 0;
	}

	if (!_mk_link(lv)) {
		stack;
		return 0;
	}

	return 1;
}

int fs_del_lv(struct logical_volume *lv)
{
	if (!_rm_link(lv)) {
		stack;
		return 0;
	}

	if (!_rm_dir(lv->vg)) {
		stack;
		return 0;
	}

	if (!_rm_node(lv)) {
		stack;
		return 0;
	}

	return 1;
}

