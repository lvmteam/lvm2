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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <linux/dm-ioctl.h>
#include <linux/kdev_t.h>

#define DEV_DIR "/dev/"
#define ALIGNMENT sizeof(int)

static char _dm_dir[PATH_MAX] = DEV_DIR DM_DIR;

/*
 * Library users can provide their own logging
 * function.
 */
static void _default_log(int level, const char *file, int line,
			 const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);

	fprintf(stderr, "\n");
}

static dm_log_fn _log = _default_log;

void dm_log_init(dm_log_fn fn)
{
	_log = fn;
}

#define log(msg, x...) _log(1, __FILE__, __LINE__, msg, ## x)

struct target {

	unsigned long long start;
	unsigned long long length;
	char *type;
	char *params;

	struct target *next;
};

struct dm_task {
	int type;
	char *dev_name;

	struct target *head, *tail;

	struct dm_ioctl *dmi;
};

struct dm_task *dm_task_create(int type)
{
	struct dm_task *dmt = malloc(sizeof(*dmt));

	if (!dmt)
		return NULL;

	memset(dmt, 0, sizeof(*dmt));

	dmt->type = type;
	return dmt;
}

void dm_task_destroy(struct dm_task *dmt)
{
	struct target *t, *n;

	for (t = dmt->head; t; t = n) {
		n = t->next;
		free(t);
	}

	if (dmt->dmi)
		free(dmt->dmi);

	free(dmt);
}

int dm_task_set_name(struct dm_task *dmt, const char *name)
{
	if (dmt->dev_name)
		free(dmt->dev_name);

	return (dmt->dev_name = strdup(name)) ? 1 : 0;
}

int dm_task_get_info(struct dm_task *dmt, struct dm_info *info)
{
	if (!dmt->dmi)
		return 0;

	info->exists = dmt->dmi->exists;
	info->suspended = dmt->dmi->suspend;
	info->open_count = dmt->dmi->open_count;
	info->major = dmt->dmi->major;
	info->minor = dmt->dmi->minor;
	info->target_count = dmt->dmi->target_count;
	return 1;
}

static struct target *_create_target(unsigned long long start,
				     unsigned long long len,
				     const char *type, const char *params)
{
	struct target *t = malloc(sizeof(*t));

	if (!t)
		return NULL;
	memset(t, 0, sizeof(*t));

	if (!(t->params = strdup(params))) {
		log("Out of memory");
		goto bad;
	}

	if (!(t->type = strdup(type))) {
		log("Out of memory");
		goto bad;
	}

	t->start = start;
	t->length = len;
	return t;

 bad:
	free(t->params);
	free(t->type);
	free(t);
	return NULL;
}

int dm_task_add_target(struct dm_task *dmt,
		       unsigned long long start,
		       unsigned long long size,
		       const char *ttype,
		       const char *params)
{
	struct target *t = _create_target(start, size, ttype, params);

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

static void *_align(void *ptr, unsigned int align)
{
	align--;
	return (void *) (((unsigned long) ptr + align) & ~align);
}

static void *_add_target(struct target *t, void *out, void *end)
{
	void *out_sp = out;
	struct dm_target_spec sp;
	int len;
	const char no_space[] = "Ran out of memory building ioctl parameter";

	out += sizeof(struct dm_target_spec);
	if (out >= end) {
		log(no_space);
		return NULL;
	}

	sp.status = 0;
	sp.sector_start = t->start;
	sp.length = t->length;
	strncpy(sp.target_type, t->type, sizeof(sp.target_type));

	len = strlen(t->params);

	if ((out + len + 1) >= end) {
		log(no_space);

		log("t->params= '%s'", t->params);
		return NULL;
	}
	strcpy((char *) out, t->params);
	out += len + 1;

	/* align next block */
	out = _align(out, ALIGNMENT);

	sp.next = out - out_sp;
	memcpy(out_sp, &sp, sizeof(sp));

	return out;
}

static struct dm_ioctl *_flatten(struct dm_task *dmt)
{
	struct dm_ioctl *dmi;
	struct target *t;
	size_t len = sizeof(struct dm_ioctl);
	void *b, *e;
	int count = 0;

	for (t = dmt->head; t; t = t->next) {
		len += sizeof(struct dm_target_spec);
		len += strlen(t->params) + 1 + ALIGNMENT;
		count++;
	}

	if (!(dmi = malloc(len)))
		return NULL;

	dmi->data_size = len;
	strncpy(dmi->name, dmt->dev_name, sizeof(dmi->name));
	dmi->suspend = (dmt->type == DM_DEVICE_SUSPEND) ? 1 : 0;
	dmi->open_count = 0;
	dmi->minor = -1;

	dmi->target_count = count;

	b = (void *) (dmi + 1);
	e = (void *) ((char *) dmi + len);

	for (t = dmt->head; t; t = t->next)
		if (!(b = _add_target(t, b, e)))
			goto bad;

	return dmi;

 bad:
	free(dmi);
	return NULL;
}

static void _build_dev_path(char *buffer, size_t len, const char *dev_name)
{
	snprintf(buffer, len, "/dev/%s/%s", DM_DIR, dev_name);
}

static int _add_dev_node(const char *dev_name, dev_t dev)
{
	char path[PATH_MAX];
	struct stat info;

	_build_dev_path(path, sizeof(path), dev_name);

	if (stat(path, &info) >= 0) {
		if (!S_ISBLK(info.st_mode)) {
			log("A non-block device file at '%s' "
			    "is already present", path);
			return 0;
		}

		if (info.st_rdev == dev)
			return 1;

		if (unlink(path) < 0) {
			log("Unable to unlink device node for '%s'", dev_name);
			return 0;
		}
	}

	if (mknod(path, S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP, dev) < 0) {
		log("Unable to make device node for '%s'", dev_name);
		return 0;
	}

	return 1;
}

static int _rm_dev_node(const char *dev_name)
{
	char path[PATH_MAX];
	struct stat info;

	_build_dev_path(path, sizeof(path), dev_name);

	if (stat(path, &info) < 0)
		return 1;

	if (unlink(path) < 0) {
		log("Unable to unlink device node for '%s'", dev_name);
		return 0;
	}

	return 1;
}

int dm_task_run(struct dm_task *dmt)
{
	int fd = -1;
	struct dm_ioctl *dmi = _flatten(dmt);
	unsigned int command;
	char control[PATH_MAX];

	if (!dmi) {
		log("Couldn't create ioctl argument");
		return 0;
	}

	snprintf(control, sizeof(control), "%s/control", _dm_dir);

	if ((fd = open(control, O_RDWR)) < 0) {
		log("Couldn't open device-mapper control device");
		goto bad;
	}

	switch (dmt->type) {
	case DM_DEVICE_CREATE:
		command = DM_CREATE;
		break;

	case DM_DEVICE_RELOAD:
		command = DM_RELOAD;
		break;

	case DM_DEVICE_REMOVE:
		command = DM_REMOVE;
		break;

	case DM_DEVICE_SUSPEND:
		command = DM_SUSPEND;
		break;

	case DM_DEVICE_RESUME:
		command = DM_SUSPEND;
		break;

	case DM_DEVICE_INFO:
		command = DM_INFO;
		break;

	default:
		log("Internal error: unknown device-mapper task %d",
		    dmt->type);
		goto bad;
	}

	if (ioctl(fd, command, dmi) < 0) {
		log("device-mapper ioctl cmd %d failed: %s", dmt->type,
		    strerror(errno));
		goto bad;
	}

	switch (dmt->type) {
	case DM_DEVICE_CREATE:
		_add_dev_node(dmt->dev_name, MKDEV(dmi->major, dmi->minor));
		break;

	case DM_DEVICE_REMOVE:
		_rm_dev_node(dmt->dev_name);
		break;
	}

	dmt->dmi = dmi;
	return 1;

 bad:
	free(dmi);
	if (fd >= 0)
		close(fd);
	return 0;
}

int dm_set_dev_dir(const char *dir)
{
	snprintf(_dm_dir, sizeof(_dm_dir), "%s/%s", dir, DM_DIR);
	return 1;
}

const char *dm_dir(void)
{
	return _dm_dir;
}
