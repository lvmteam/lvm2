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
#include <errno.h>
#include <linux/kdev_t.h>

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

#include "libdm-targets.h"
#include "libdm-common.h"

#define ALIGNMENT sizeof(int)

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

struct target *create_target(uint64_t start,
				     uint64_t len,
				     const char *type, const char *params)
{
	struct target *t = malloc(sizeof(*t));

	if (!t) {
                log("create_target: malloc(%d) failed", sizeof(*t));
		return NULL;
	}

	memset(t, 0, sizeof(*t));

	if (!(t->params = strdup(params))) {
		log("create_target: strdup(params) failed");
		goto bad;
	}

	if (!(t->type = strdup(type))) {
		log("create_target: strdup(type) failed");
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

	strncpy(dmi->version, DM_IOCTL_VERSION, sizeof(dmi->version));
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

	snprintf(control, sizeof(control), "%s/control", dm_dir());

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
		add_dev_node(dmt->dev_name, MKDEV(dmi->major, dmi->minor));
		break;

	case DM_DEVICE_REMOVE:
		rm_dev_node(dmt->dev_name);
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

