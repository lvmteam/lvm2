/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "libdevmapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <linux/kdev_t.h>
#include <linux/device-mapper.h>

#include "libdm-targets.h"
#include "libdm-common.h"

#define DEV_DIR "/dev/"

static char _dm_dir[PATH_MAX] = DEV_DIR DM_DIR;

/*
 * Library users can provide their own logging
 * function.
 */
void _default_log(int level, const char *file, int line, const char *f, ...)
{
	va_list ap;

	if (level > _LOG_WARN)
		return;

	va_start(ap, f);

	if (level == _LOG_WARN)
		vprintf(f, ap);
	else
		vfprintf(stderr, f, ap);

	va_end(ap);

	fprintf(stderr, "\n");
}

dm_log_fn _log = _default_log;

void dm_log_init(dm_log_fn fn)
{
	_log = fn;
}

void _build_dev_path(char *buffer, size_t len, const char *dev_name)
{
	/* If there's a /, assume caller knows what they're doing */
	if (strchr(dev_name, '/'))
		snprintf(buffer, len, "%s", dev_name);
	else
		snprintf(buffer, len, "%s/%s", _dm_dir, dev_name);
}

int dm_get_library_version(char *version, size_t size)
{
	strncpy(version, DM_LIB_VERSION, size);
	return 1;
}

struct dm_task *dm_task_create(int type)
{
	struct dm_task *dmt = malloc(sizeof(*dmt));

	if (!dmt) {
		log_error("dm_task_create: malloc(%d) failed", sizeof(*dmt));
		return NULL;
	}

	memset(dmt, 0, sizeof(*dmt));

	dmt->type = type;
	dmt->minor = -1;
	return dmt;
}

int dm_task_set_name(struct dm_task *dmt, const char *name)
{
	char *pos;
	char path[PATH_MAX];
	struct stat st1, st2;

	if (dmt->dev_name) {
		free(dmt->dev_name);
		dmt->dev_name = NULL;
	}

	/* If path was supplied, remove it if it points to the same device
	 * as its last component.
	 */
	if ((pos = strrchr(name, '/'))) {
		snprintf(path, sizeof(path), "%s/%s", _dm_dir, pos + 1);

		if (stat(name, &st1) || stat(path, &st2) ||
		    !(st1.st_dev == st2.st_dev)) {
			log_error("dm_task_set_name: Device %s not found",
				  name);
			return 0;
		}

		name = pos + 1;
	}

	if (!(dmt->dev_name = strdup(name))) {
		log_error("dm_task_set_name: strdup(%s) failed", name);
		return 0;
	}

	return 1;
}

int dm_task_set_uuid(struct dm_task *dmt, const char *uuid)
{
	if (dmt->uuid) {
		free(dmt->uuid);
		dmt->uuid = NULL;
	}

	if (!(dmt->uuid = strdup(uuid))) {
		log_error("dm_task_set_uuid: strdup(%s) failed", uuid);
		return 0;
	}

	return 1;
}

int dm_task_set_minor(struct dm_task *dmt, int minor)
{
	dmt->minor = minor;
	log_debug("Setting minor: %d", dmt->minor);

	return 1;
}

int dm_task_add_target(struct dm_task *dmt, uint64_t start, uint64_t size,
		       const char *ttype, const char *params)
{
	struct target *t = create_target(start, size, ttype, params);

	if (!t)
		return 0;

	if (!dmt->head)
		dmt->head = dmt->tail = t;
	else {
		dmt->tail->next = t;
		dmt->tail = t;
	}

	return 1;
}

int add_dev_node(const char *dev_name, dev_t dev)
{
	char path[PATH_MAX];
	struct stat info;

	_build_dev_path(path, sizeof(path), dev_name);

	if (stat(path, &info) >= 0) {
		if (!S_ISBLK(info.st_mode)) {
			log_error("A non-block device file at '%s' "
				  "is already present", path);
			return 0;
		}

		if (info.st_rdev == dev)
			return 1;

		if (unlink(path) < 0) {
			log_error("Unable to unlink device node for '%s'",
				  dev_name);
			return 0;
		}
	}

	if (mknod(path, S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP, dev) < 0) {
		log_error("Unable to make device node for '%s'", dev_name);
		return 0;
	}

	return 1;
}

int rename_dev_node(const char *old_name, const char *new_name)
{
	char oldpath[PATH_MAX];
	char newpath[PATH_MAX];
	struct stat info;

	_build_dev_path(oldpath, sizeof(oldpath), old_name);
	_build_dev_path(newpath, sizeof(newpath), new_name);

	if (stat(newpath, &info) == 0) {
		if (!S_ISBLK(info.st_mode)) {
			log_error("A non-block device file at '%s' "
				  "is already present", newpath);
			return 0;
		}

		if (unlink(newpath) < 0) {
			if (errno == EPERM) {	
				/* devfs, entry has already been renamed */
				return 1;
			}
			log_error("Unable to unlink device node for '%s'",
				  new_name);
			return 0;
		}
	}

	if (rename(oldpath, newpath) < 0) {
		log_error("Unable to rename device node from '%s' to '%s'",
			  old_name, new_name);
		return 0;
	}

	return 1;
}

int rm_dev_node(const char *dev_name)
{
	char path[PATH_MAX];
	struct stat info;

	_build_dev_path(path, sizeof(path), dev_name);

	if (stat(path, &info) < 0)
		return 1;

	if (unlink(path) < 0) {
		log_error("Unable to unlink device node for '%s'", dev_name);
		return 0;
	}

	return 1;
}

int dm_set_dev_dir(const char *dir)
{
	snprintf(_dm_dir, sizeof(_dm_dir), "%s%s", dir, DM_DIR);
	return 1;
}

const char *dm_dir(void)
{
	return _dm_dir;
}
