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

#include <libdevmapper.h>


void _build_lv_path(char *buffer, size_t len, struct logical_volume *lv,
		    const char *lv_name)
{
	snprintf(buffer, len, "%s/%s_%s", dm_dir(), lv->vg->name, lv_name);
}

void _build_vg_path(char *buffer, size_t len, struct volume_group *vg)
{
	snprintf(buffer, len, "%s%s", vg->cmd->dev_dir, vg->name);
}

void _build_link_path(char *buffer, size_t len, struct logical_volume *lv,
		      const char *lv_name)
{
	snprintf(buffer, len, "%s%s/%s", lv->vg->cmd->dev_dir,
		 lv->vg->name, lv_name);
}

/*
 * Lazy programmer: I'm just going to always try
 * and create/remove the vg directory, and not say
 * anything if it fails.
 */
static void _mk_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	_build_vg_path(vg_path, sizeof(vg_path), vg);

	log_very_verbose("Creating directory %s", vg_path);
	mkdir(vg_path, 0555);
}

static void _rm_dir(struct volume_group *vg)
{
	char vg_path[PATH_MAX];

	_build_vg_path(vg_path, sizeof(vg_path), vg);

	log_very_verbose("Removing directory %s", vg_path);
	rmdir(vg_path);
}

static int _mk_link(struct logical_volume *lv)
{
	char lv_path[PATH_MAX], link_path[PATH_MAX];

	_build_lv_path(lv_path, sizeof(lv_path), lv, lv->name);
	_build_link_path(link_path, sizeof(link_path), lv, lv->name);

	log_very_verbose("Linking %s to %s", link_path, lv_path);
	if (symlink(lv_path, link_path) < 0) {
		log_sys_error("symlink", link_path);
		return 0;
	}

	return 1;
}

static int _rm_link(struct logical_volume *lv, const char *lv_name)
{
	char link_path[PATH_MAX];

	if (!lv_name)
		lv_name = lv->name;

	_build_link_path(link_path, sizeof(link_path), lv, lv_name);

	log_very_verbose("Removing link %s", link_path);
	if (unlink(link_path) < 0) {
		log_sys_error("unlink", link_path);
		return 0;
	}

	return 1;
}

int fs_add_lv(struct logical_volume *lv)
{
	_mk_dir(lv->vg);

	if (!_mk_link(lv)) {
		stack;
		return 0;
	}

	return 1;
}

int fs_del_lv(struct logical_volume *lv)
{
	if (!_rm_link(lv, NULL)) {
		stack;
		return 0;
	}

	_rm_dir(lv->vg);

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
