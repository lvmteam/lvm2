/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include <unistd.h>

/*
 * FIXME: copied straight from LVM1, we need to
 * check awkward cases like the guy who was
 * running devfs in parallel, mounted on
 * /devfs/
 *
 * This should run through /proc/mounts once only,
 * storing devfs mount points in a hash table.
 */
static int _check_devfs(const char *dev_prefix)
{
	int r = 0, len;
	char dir[NAME_LEN], line[512];
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
			ret = 1;
			break;
		}
	}
	fclose(mounts);

 out:
	return r;
}

static inline int _running_devfs(struct io_space *ios)
{
	static int _using_devfs = -1;

	if (_using_devfs < 0)
		_using_devfs = _check_devfs(ios->prefix);

	return _using_devfs;
}

static int _mk_node(struct io_space *ios, struct logical_volume *lv)
{
	char lv_path[PATH_MAX];
	dev_t dev;

	if (_running_devfs(ios))
		return 1;

	snprintf(lv_path, sizeof(lv_path), "%s/device-mapper/%s",
		 ios->prefix, lv->name);

	if (mknod(lv_path, S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP, dev) < 0) {
		log_sys_err("mknod", "creating lv device node");
		return 0;
	}

	return 1;
}

static int _rm_node(struct io_space *ios, struct logical_volume *lv)
{
	char lv_path[PATH_MAX];

	if (_running_devfs(ios))
		return 1;

	snprintf(lv_path, sizeof(lv_path), "%s/device-mapper/%s",
		 ios->prefix, lv->name);

	if (unlink(lv_path) < 0) {
		log_sys_err("unlink", "removing lv device node");
		return 0;
	}

	return 1;
}

/*
 * Lazy programmer: I'm just going to always try
 * and create/remove the vg directory, and not say
 * anything if it fails.
 */
static int _mk_dir(struct io_space *ios, const char *vg_name)
{
	char vg_path[PATH_MAX];

	snprintf(vg_path, sizeof(vg_path), "%s/%s", ios->prefix, vg_name);
	mkdir(vg_path, 0555);
	return 1;
}

static int _rm_dir(struct io_space *ios, const char *vg_name)
{
	char vg_path[PATH_MAX];

	snprintf(vg_path, sizeof(vg_path), "%s/%s", ios->prefix, vg_name);
	rmdir(vg_path);
	return 1;
}

static int _mk_link(struct io_space *ios, struct logical_volume *lv)
{
	char lv_path[PATH_MAX], link_path[PATH_MAX];

	snprintf(lv_path, sizeof(lv_path), );
}

int fs_add_lv(struct io_space *ios, struct logical_volume *lv)
{
	if (!_mk_node(ios, lv)) {
		stack;
		return 0;
	}

	if (!_mk_dir(ios, lv->vg->name)) {
		stack;
		return 0;
	}

	if (!_mk_link(ios, lv)) {
		stack;
		return 0;
	}

	return 1;
}

int fs_del_lv(struct logical_volume *lv)
{
	if (!_rm_link(ios, lv)) {
		stack;
		return 0;
	}

	if (!_rm_dir(ios, lv->vg->name)) {
		stack;
		return 0;
	}

	if (!_rm_node(ios, lv)) {
		stack;
		return 0;
	}

	return 1;
}

