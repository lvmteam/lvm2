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
#include <errno.h>
#include <linux/kdev_t.h>

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>

#include "libdm-targets.h"
#include "libdm-common.h"

#define ALIGNMENT sizeof(int)

static char *dm_cmd_list[] = {
	"create",
	"reload",
	"remove",
	"remove_all",
	"suspend",
	"resume",
	"info",
	"deps",
	"rename",
	"version",
	"status",
	"table",
	"waitevent"
};


static void *_align(void *ptr, unsigned int a)
{
	register unsigned long align = --a;

	return (void *) (((unsigned long) ptr + align) & ~align);
}

void dm_task_destroy(struct dm_task *dmt)
{
	struct target *t, *n;

	for (t = dmt->head; t; t = n) {
		n = t->next;
		free(t->params);
		free(t->type);
		free(t);
	}

	if (dmt->dev_name)
		free(dmt->dev_name);

	if (dmt->newname)
		free(dmt->newname);

	if (dmt->dmi)
		free(dmt->dmi);

	if (dmt->uuid)
		free(dmt->uuid);

	free(dmt);
}

int dm_task_get_driver_version(struct dm_task *dmt, char *version, size_t size)
{
	if (!dmt->dmi)
		return 0;

	strncpy(version, dmt->dmi->version, size);

	return 1;
}

void *dm_get_next_target(struct dm_task *dmt, void *next, unsigned long long *start,
			 unsigned long long *length,
			 char **target_type,
			 char **params)
{
    struct target *t;

    if (!next)
	next = dmt->head;

    t = next;

    if (t) {

	*start = t->start;
	*length = t->length;
	*target_type = t->type;
	*params = t->params;
	return t->next;
    }
    return NULL;
}

/* Unmarshall the target info returned from a status call */
static int unmarshal_status(struct dm_task *dmt, struct dm_ioctl *dmi)
{
        char *outbuf = (char *)dmi + sizeof(struct dm_ioctl);
	char *outptr = outbuf;
	int i;

	for (i=0; i < dmi->target_count; i++) {
	    struct dm_target_spec *spec = (struct dm_target_spec *)outptr;

	    if (!dm_task_add_target(dmt, spec->sector_start, spec->length,
				    spec->target_type, outptr+sizeof(*spec)))
		return 0;
	    outptr += sizeof(struct dm_target_spec);
	    outptr += strlen(outptr) + 1;
	    _align(outptr, ALIGNMENT);
	}
	return 1;
}

int dm_task_get_info(struct dm_task *dmt, struct dm_info *info)
{
	if (!dmt->dmi)
		return 0;

	memset(info, 0, sizeof(*info));

	info->exists = dmt->dmi->flags & DM_EXISTS_FLAG ? 1 : 0;
	if (!info->exists)
		return 1;

	info->suspended = dmt->dmi->flags & DM_SUSPEND_FLAG ? 1 : 0;
	info->read_only = dmt->dmi->flags & DM_READONLY_FLAG ? 1 : 0;
	info->target_count = dmt->dmi->target_count;
	info->open_count = dmt->dmi->open_count;
	info->major = MAJOR(dmt->dmi->dev);
	info->minor = MINOR(dmt->dmi->dev);

	return 1;
}

const char *dm_task_get_uuid(struct dm_task *dmt)
{
	return (dmt->dmi->uuid);
}

struct dm_deps *dm_task_get_deps(struct dm_task *dmt)
{
	return (struct dm_deps *) (((void *) dmt->dmi) + dmt->dmi->data_start);
}

int dm_task_set_ro(struct dm_task *dmt)
{
	dmt->read_only = 1;
	return 1;
}

int dm_task_set_newname(struct dm_task *dmt, const char *newname)
{
	if (!(dmt->newname = strdup(newname))) {
		log_error("dm_task_set_newname: strdup(%s) failed", newname);
		return 0;
	}

	return 1;
}

struct target *create_target(uint64_t start,
			     uint64_t len, const char *type, const char *params)
{
	struct target *t = malloc(sizeof(*t));

	if (!t) {
		log_error("create_target: malloc(%d) failed", sizeof(*t));
		return NULL;
	}

	memset(t, 0, sizeof(*t));

	if (!(t->params = strdup(params))) {
		log_error("create_target: strdup(params) failed");
		goto bad;
	}

	if (!(t->type = strdup(type))) {
		log_error("create_target: strdup(type) failed");
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

static void *_add_target(struct target *t, void *out, void *end)
{
	void *out_sp = out;
	struct dm_target_spec sp;
	int len;
	const char no_space[] = "Ran out of memory building ioctl parameter";

	out += sizeof(struct dm_target_spec);
	if (out >= end) {
		log_error(no_space);
		return NULL;
	}

	sp.status = 0;
	sp.sector_start = t->start;
	sp.length = t->length;
	strncpy(sp.target_type, t->type, sizeof(sp.target_type));

	len = strlen(t->params);

	if ((out + len + 1) >= end) {
		log_error(no_space);

		log_error("t->params= '%s'", t->params);
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
	const size_t min_size = 16 * 1024;

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

	if (count && dmt->newname) {
		log_error("targets and newname are incompatible");
		return NULL;
	}

	if (dmt->newname)
		len += strlen(dmt->newname) + 1;

	/*
	 * Give len a minimum size so that we have space to store
	 * dependencies or status information.
	 */
	if (len < min_size)
		len = min_size;

	if (!(dmi = malloc(len)))
		return NULL;

	memset(dmi, 0, len);

	strncpy(dmi->version, DM_IOCTL_VERSION, sizeof(dmi->version));
	dmi->data_size = len;
	dmi->data_start = sizeof(struct dm_ioctl);

	if (dmt->dev_name)
		strncpy(dmi->name, dmt->dev_name, sizeof(dmi->name));

	if (dmt->type == DM_DEVICE_SUSPEND)
		dmi->flags |= DM_SUSPEND_FLAG;
	if (dmt->read_only)
		dmi->flags |= DM_READONLY_FLAG;

	if (dmt->minor >= 0) {
		dmi->flags |= DM_PERSISTENT_DEV_FLAG;
		dmi->dev = MKDEV(0, dmt->minor);
	}

	if (dmt->uuid)
		strncpy(dmi->uuid, dmt->uuid, sizeof(dmi->uuid));

	dmi->target_count = count;

	b = (void *) (dmi + 1);
	e = (void *) ((char *) dmi + len);

	for (t = dmt->head; t; t = t->next)
		if (!(b = _add_target(t, b, e)))
			goto bad;

	if (dmt->newname)
		strcpy(b, dmt->newname);

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
		log_error("Couldn't create ioctl argument");
		return 0;
	}

	snprintf(control, sizeof(control), "%s/control", dm_dir());

	if ((fd = open(control, O_RDWR)) < 0) {
		log_error("Couldn't open device-mapper control device");
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

	case DM_DEVICE_REMOVE_ALL:
		command = DM_REMOVE_ALL;
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

	case DM_DEVICE_DEPS:
		command = DM_DEPS;
		break;

	case DM_DEVICE_RENAME:
		command = DM_RENAME;
		break;

	case DM_DEVICE_VERSION:
		command = DM_VERSION;
		break;

	case DM_DEVICE_STATUS:
		command = DM_GET_STATUS;
		break;

	case DM_DEVICE_TABLE:
	        dmi->flags |= DM_STATUS_TABLE_FLAG;
		command = DM_GET_STATUS;
		break;

	case DM_DEVICE_WAITEVENT:
	        command = DM_WAIT_EVENT;
		break;

	default:
		log_error("Internal error: unknown device-mapper task %d",
			  dmt->type);
		goto bad;
	}

	log_debug("dm %s %s %s %s", dm_cmd_list[dmt->type], dmi->name,
		  dmi->uuid, dmt->newname ? dmt->newname : "");
	if (ioctl(fd, command, dmi) < 0) {
		log_error("device-mapper ioctl cmd %d failed: %s", dmt->type,
			  strerror(errno));
		goto bad;
	}

	switch (dmt->type) {
	case DM_DEVICE_CREATE:
		add_dev_node(dmt->dev_name, dmi->dev);
		break;

	case DM_DEVICE_REMOVE:
		rm_dev_node(dmt->dev_name);
		break;

	case DM_DEVICE_RENAME:
		rename_dev_node(dmt->dev_name, dmt->newname);
		break;

	case DM_DEVICE_STATUS:
	case DM_DEVICE_TABLE:
	        if (!unmarshal_status(dmt, dmi))
		        goto bad;
	        break;
	}

	dmt->dmi = dmi;
	close(fd);
	return 1;

      bad:
	free(dmi);
	if (fd >= 0)
		close(fd);
	return 0;
}
