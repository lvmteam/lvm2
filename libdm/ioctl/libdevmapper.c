/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "libdm-targets.h"
#include "libdm-common.h"
#include "libdm-compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/limits.h>

/*
 * Ensure build compatibility.  
 * The hard-coded versions here are the highest present 
 * in the _cmd_data arrays.
 */

#if !((DM_VERSION_MAJOR == 1 && DM_VERSION_MINOR >= 0) || \
      (DM_VERSION_MAJOR == 3 && DM_VERSION_MINOR >= 0))
#error The version of dm-ioctl.h included is incompatible.
#endif

/* dm major version no for running kernel */
static int _dm_version = DM_VERSION_MAJOR;
static int _log_suppress = 0;

/*
 * Support both old and new major numbers to ease the transition.
 * Clumsy, but only temporary.
 */
#if DM_VERSION_MAJOR == 3 && defined(DM_COMPAT)
const int _dm_compat = 1;
#else
const int _dm_compat = 0;
#endif


/* *INDENT-OFF* */
static struct cmd_data _cmd_data_v3[] = {
	{"create",	DM_DEV_CREATE,		{3, 0, 0}},
	{"reload",	DM_DEV_RELOAD,		{3, 0, 0}},
	{"remove",	DM_DEV_REMOVE,		{3, 0, 0}},
	{"remove_all",	DM_REMOVE_ALL,		{3, 0, 0}},
	{"suspend",	DM_DEV_SUSPEND,		{3, 0, 0}},
	{"resume",	DM_DEV_SUSPEND,		{3, 0, 0}},
	{"info",	DM_DEV_STATUS,		{3, 0, 0}},
	{"deps",	DM_DEV_DEPS,		{3, 0, 0}},
	{"rename",	DM_DEV_RENAME,		{3, 0, 0}},
	{"version",	DM_VERSION,		{3, 0, 0}},
	{"status",	DM_TARGET_STATUS,	{3, 0, 0}},
	{"table",	DM_TARGET_STATUS,	{3, 0, 0}},
	{"waitevent",	DM_TARGET_WAIT,		{3, 0, 0}},
};
/* *INDENT-ON* */

#define ALIGNMENT_V1 sizeof(int)
#define ALIGNMENT 8

/* FIXME Rejig library to record & use errno instead */
#ifndef DM_EXISTS_FLAG
#  define DM_EXISTS_FLAG 0x00000004
#endif

static void *_align(void *ptr, unsigned int a)
{
	register unsigned long agn = --a;

	return (void *) (((unsigned long) ptr + agn) & ~agn);
}

static int _dm_task_get_driver_version_v1(struct dm_task *dmt, char *version,
					  size_t size)
{
	unsigned int *v;

	if (!dmt->dmi.v1) {
		version[0] = '\0';
		return 0;
	}

	v = dmt->dmi.v1->version;
	snprintf(version, size, "%u.%u.%u", v[0], v[1], v[2]);
	return 1;
}

/* Unmarshall the target info returned from a status call */
static int _unmarshal_status_v1(struct dm_task *dmt, struct dm_ioctl_v1 *dmi)
{
	char *outbuf = (char *) dmi + dmi->data_start;
	char *outptr = outbuf;
	int32_t i;
	struct dm_target_spec_v1 *spec;

	for (i = 0; i < dmi->target_count; i++) {
		spec = (struct dm_target_spec_v1 *) outptr;

		if (!dm_task_add_target(dmt, spec->sector_start,
					(uint64_t) spec->length,
					spec->target_type,
					outptr + sizeof(*spec)))
			return 0;

		outptr = outbuf + spec->next;
	}

	return 1;
}

static int _dm_task_get_info_v1(struct dm_task *dmt, struct dm_info *info)
{
	if (!dmt->dmi.v1)
		return 0;

	memset(info, 0, sizeof(*info));

	info->exists = dmt->dmi.v1->flags & DM_EXISTS_FLAG ? 1 : 0;
	if (!info->exists)
		return 1;

	info->suspended = dmt->dmi.v1->flags & DM_SUSPEND_FLAG ? 1 : 0;
	info->read_only = dmt->dmi.v1->flags & DM_READONLY_FLAG ? 1 : 0;
	info->target_count = dmt->dmi.v1->target_count;
	info->open_count = dmt->dmi.v1->open_count;
	info->event_nr = 0;
	info->major = MAJOR(dmt->dmi.v1->dev);
	info->minor = MINOR(dmt->dmi.v1->dev);

	return 1;
}

static const char *_dm_task_get_uuid_v1(struct dm_task *dmt)
{
	return (dmt->dmi.v1->uuid);
}

static struct dm_deps *_dm_task_get_deps_v1(struct dm_task *dmt)
{
	log_error("deps version 1 no longer supported by libdevmapper");
	return NULL;
}

static void *_add_target_v1(struct target *t, void *out, void *end)
{
	void *out_sp = out;
	struct dm_target_spec_v1 sp;
	size_t sp_size = sizeof(struct dm_target_spec_v1);
	int len;
	const char no_space[] = "Ran out of memory building ioctl parameter";

	out += sp_size;
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
	out = _align(out, ALIGNMENT_V1);

	sp.next = out - out_sp;

	memcpy(out_sp, &sp, sp_size);

	return out;
}

static struct dm_ioctl_v1 *_flatten_v1(struct dm_task *dmt)
{
	const size_t min_size = 16 * 1024;
	const int (*version)[3];

	struct dm_ioctl_v1 *dmi;
	struct target *t;
	size_t len = sizeof(struct dm_ioctl_v1);
	void *b, *e;
	int count = 0;

	for (t = dmt->head; t; t = t->next) {
		len += sizeof(struct dm_target_spec_v1);
		len += strlen(t->params) + 1 + ALIGNMENT_V1;
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

	version = &_cmd_data_v1[dmt->type].version;

	dmi->version[0] = (*version)[0];
	dmi->version[1] = (*version)[1];
	dmi->version[2] = (*version)[2];

	dmi->data_size = len;
	dmi->data_start = sizeof(struct dm_ioctl_v1);

	if (dmt->dev_name)
		strncpy(dmi->name, dmt->dev_name, sizeof(dmi->name));

	if (dmt->type == DM_DEVICE_SUSPEND)
		dmi->flags |= DM_SUSPEND_FLAG;
	if (dmt->read_only)
		dmi->flags |= DM_READONLY_FLAG;

	if (dmt->minor >= 0) {
		if (dmt->major <= 0) {
			log_error("Missing major number for persistent device");
			return NULL;
		}
		dmi->flags |= DM_PERSISTENT_DEV_FLAG;
		dmi->dev = MKDEV(dmt->major, dmt->minor);
	}

	if (dmt->uuid)
		strncpy(dmi->uuid, dmt->uuid, sizeof(dmi->uuid));

	dmi->target_count = count;

	b = (void *) (dmi + 1);
	e = (void *) ((char *) dmi + len);

	for (t = dmt->head; t; t = t->next)
		if (!(b = _add_target_v1(t, b, e)))
			goto bad;

	if (dmt->newname)
		strcpy(b, dmt->newname);

	return dmi;

      bad:
	free(dmi);
	return NULL;
}

static int _dm_task_run_v1(struct dm_task *dmt)
{
	int fd = -1;
	struct dm_ioctl_v1 *dmi;
	unsigned int command;
	char control[PATH_MAX];

	dmi = _flatten_v1(dmt);
	if (!dmi) {
		log_error("Couldn't create ioctl argument");
		return 0;
	}

	snprintf(control, sizeof(control), "%s/control", dm_dir());

	if ((fd = open(control, O_RDWR)) < 0) {
		log_error("%s: open failed: %s", control, strerror(errno));
		log_error("Is device-mapper driver missing from kernel?");
		goto bad;
	}

	if ((unsigned) dmt->type >=
	    (sizeof(_cmd_data_v1) / sizeof(*_cmd_data_v1))) {
		log_error("Internal error: unknown device-mapper task %d",
			  dmt->type);
		goto bad;
	}

	command = _cmd_data_v1[dmt->type].cmd;

	if (dmt->type == DM_DEVICE_TABLE)
		dmi->flags |= DM_STATUS_TABLE_FLAG;

	log_debug("dm %s %s %s %s", _cmd_data_v1[dmt->type].name, dmi->name,
		  dmi->uuid, dmt->newname ? dmt->newname : "");
	if (ioctl(fd, command, dmi) < 0) {
		if (_log_suppress)
			log_verbose("device-mapper ioctl cmd %d failed: %s",
				    _IOC_NR(command), strerror(errno));
		else
			log_error("device-mapper ioctl cmd %d failed: %s",
				  _IOC_NR(command), strerror(errno));
		goto bad;
	}

	switch (dmt->type) {
	case DM_DEVICE_CREATE:
		add_dev_node(dmt->dev_name, MAJOR(dmi->dev), MINOR(dmi->dev));
		break;

	case DM_DEVICE_REMOVE:
		rm_dev_node(dmt->dev_name);
		break;

	case DM_DEVICE_RENAME:
		rename_dev_node(dmt->dev_name, dmt->newname);
		break;

	case DM_DEVICE_STATUS:
	case DM_DEVICE_TABLE:
		if (!_unmarshal_status_v1(dmt, dmi))
			goto bad;
		break;
	}

	dmt->dmi.v1 = dmi;
	close(fd);
	return 1;

      bad:
	free(dmi);
	if (fd >= 0)
		close(fd);
	return 0;
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

	if (dmt->dmi.v3)
		free(dmt->dmi.v3);

	if (dmt->uuid)
		free(dmt->uuid);

	free(dmt);
}

int dm_task_get_driver_version(struct dm_task *dmt, char *version, size_t size)
{
	unsigned int *v;

	if (_dm_version == 1)
		return _dm_task_get_driver_version_v1(dmt, version, size);

	if (!dmt->dmi.v3) {
		version[0] = '\0';
		return 0;
	}

	v = dmt->dmi.v3->version;
	snprintf(version, size, "%u.%u.%u", v[0], v[1], v[2]);
	return 1;
}

static int _check_version(char *version, size_t size, int log_suppress)
{
	struct dm_task *task;
	int r;

	if (!(task = dm_task_create(DM_DEVICE_VERSION))) {
		log_error("Failed to get device-mapper version");
		version[0] = '\0';
		return 0;
	}

	if (log_suppress)
		_log_suppress = 1;

	r = dm_task_run(task);
	dm_task_get_driver_version(task, version, size);
	dm_task_destroy(task);
	_log_suppress = 0;

	return r;
}

/*
 * Find out device-mapper's major version number the first time 
 * this is called and whether or not we support it.
 */
int dm_check_version(void)
{
	static int _checked = 0;
	static int _ok = 1;
	char libversion[64], dmversion[64];
	const char *compat = "";

	if (_checked)
		return _ok;

	_checked = 1;

	if (_check_version(dmversion, sizeof(dmversion), _dm_compat))
		return 1;

	if (!_dm_compat)
		goto bad;

	log_verbose("device-mapper ioctl protocol version %d failed. "
		    "Trying protocol version 1.", _dm_version);
	_dm_version = 1;
	if (_check_version(dmversion, sizeof(dmversion), 0)) {
		log_verbose("Using device-mapper ioctl protocol version 1");
		return 1;
	}

	compat = "(compat)";

	dm_get_library_version(libversion, sizeof(libversion));

	log_error("Incompatible libdevmapper %s%s and kernel driver %s",
		  libversion, compat, dmversion);

      bad:
	_ok = 0;
	return 0;
}

void *dm_get_next_target(struct dm_task *dmt, void *next,
			 uint64_t *start, uint64_t *length,
			 char **target_type, char **params)
{
	struct target *t = (struct target *) next;

	if (!t)
		t = dmt->head;

	if (!t)
		return NULL;

	*start = t->start;
	*length = t->length;
	*target_type = t->type;
	*params = t->params;

	return t->next;
}

/* Unmarshall the target info returned from a status call */
static int _unmarshal_status(struct dm_task *dmt, struct dm_ioctl *dmi)
{
	char *outbuf = (char *) dmi + dmi->data_start;
	char *outptr = outbuf;
	uint32_t i;
	struct dm_target_spec *spec;

	for (i = 0; i < dmi->target_count; i++) {
		spec = (struct dm_target_spec *) outptr;
		if (!dm_task_add_target(dmt, spec->sector_start,
					spec->length,
					spec->target_type,
					outptr + sizeof(*spec)))
			return 0;

		outptr = outbuf + spec->next;
	}

	return 1;
}

int dm_task_get_info(struct dm_task *dmt, struct dm_info *info)
{
	if (_dm_version == 1)
		return _dm_task_get_info_v1(dmt, info);

	if (!dmt->dmi.v3)
		return 0;

	memset(info, 0, sizeof(*info));

	info->exists = dmt->dmi.v3->flags & DM_EXISTS_FLAG ? 1 : 0;
	if (!info->exists)
		return 1;

	info->suspended = dmt->dmi.v3->flags & DM_SUSPEND_FLAG ? 1 : 0;
	info->read_only = dmt->dmi.v3->flags & DM_READONLY_FLAG ? 1 : 0;
	info->target_count = dmt->dmi.v3->target_count;
	info->open_count = dmt->dmi.v3->open_count;
	info->event_nr = dmt->dmi.v3->event_nr;
	info->major = MAJOR(dmt->dmi.v3->dev);
	info->minor = MINOR(dmt->dmi.v3->dev);

	return 1;
}

const char *dm_task_get_uuid(struct dm_task *dmt)
{
	if (_dm_version == 1)
		return _dm_task_get_uuid_v1(dmt);

	return (dmt->dmi.v3->uuid);
}

struct dm_deps *dm_task_get_deps(struct dm_task *dmt)
{
	if (_dm_version == 1)
		return _dm_task_get_deps_v1(dmt);

	return (struct dm_deps *) (((void *) dmt->dmi.v3) +
				   dmt->dmi.v3->data_start);
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

int dm_task_set_event_nr(struct dm_task *dmt, uint32_t event_nr)
{
	dmt->event_nr = event_nr;

	return 1;
}

struct target *create_target(uint64_t start, uint64_t len, const char *type,
			     const char *params)
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
	size_t sp_size = sizeof(struct dm_target_spec);
	int len;
	const char no_space[] = "Ran out of memory building ioctl parameter";

	out += sp_size;
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
	memcpy(out_sp, &sp, sp_size);

	return out;
}

static struct dm_ioctl *_flatten(struct dm_task *dmt)
{
	const size_t min_size = 16 * 1024;
	const int (*version)[3];

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

	version = &_cmd_data_v3[dmt->type].version;

	dmi->version[0] = (*version)[0];
	dmi->version[1] = (*version)[1];
	dmi->version[2] = (*version)[2];

	dmi->data_size = len;
	dmi->data_start = sizeof(struct dm_ioctl);

	if (dmt->dev_name)
		strncpy(dmi->name, dmt->dev_name, sizeof(dmi->name));

	if (dmt->type == DM_DEVICE_SUSPEND)
		dmi->flags |= DM_SUSPEND_FLAG;
	if (dmt->read_only)
		dmi->flags |= DM_READONLY_FLAG;

	if (dmt->minor >= 0) {
		if (dmt->major <= 0) {
			log_error("Missing major number for persistent device");
			return NULL;
		}
		dmi->flags |= DM_PERSISTENT_DEV_FLAG;
		dmi->dev = MKDEV(dmt->major, dmt->minor);
	}

	if (dmt->uuid)
		strncpy(dmi->uuid, dmt->uuid, sizeof(dmi->uuid));

	dmi->target_count = count;
	dmi->event_nr = dmt->event_nr;

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
	struct dm_ioctl *dmi;
	unsigned int command;
	char control[PATH_MAX];

	if (_dm_version == 1)
		return _dm_task_run_v1(dmt);

	dmi = _flatten(dmt);
	if (!dmi) {
		log_error("Couldn't create ioctl argument");
		return 0;
	}

	snprintf(control, sizeof(control), "%s/control", dm_dir());

	if ((fd = open(control, O_RDWR)) < 0) {
		log_error("%s: open failed: %s", control, strerror(errno));
		log_error("Is device-mapper driver missing from kernel?");
		goto bad;
	}

	if ((unsigned) dmt->type >=
	    (sizeof(_cmd_data_v3) / sizeof(*_cmd_data_v3))) {
		log_error("Internal error: unknown device-mapper task %d",
			  dmt->type);
		goto bad;
	}

	command = _cmd_data_v3[dmt->type].cmd;

	if (dmt->type == DM_DEVICE_TABLE)
		dmi->flags |= DM_STATUS_TABLE_FLAG;

	dmi->flags |= DM_EXISTS_FLAG;	/* FIXME */
	log_debug("dm %s %s %s %s", _cmd_data_v3[dmt->type].name, dmi->name,
		  dmi->uuid, dmt->newname ? dmt->newname : "");
	if (ioctl(fd, command, dmi) < 0) {
		if (errno == ENXIO && dmt->type == DM_DEVICE_INFO) {
			dmi->flags &= ~DM_EXISTS_FLAG;	/* FIXME */
			goto ignore_error;
		}
		if (_log_suppress)
			log_verbose("device-mapper ioctl cmd %d failed: %s",
				    _IOC_NR(command), strerror(errno));
		else
			log_error("device-mapper ioctl cmd %d failed: %s",
				  _IOC_NR(command), strerror(errno));
		goto bad;
	}

      ignore_error:
	switch (dmt->type) {
	case DM_DEVICE_CREATE:
		add_dev_node(dmt->dev_name, MAJOR(dmi->dev), MINOR(dmi->dev));
		break;

	case DM_DEVICE_REMOVE:
		rm_dev_node(dmt->dev_name);
		break;

	case DM_DEVICE_RENAME:
		rename_dev_node(dmt->dev_name, dmt->newname);
		break;

	case DM_DEVICE_STATUS:
	case DM_DEVICE_TABLE:
	case DM_DEVICE_WAITEVENT:
		if (!_unmarshal_status(dmt, dmi))
			goto bad;
		break;
	}

	dmt->dmi.v3 = dmi;
	close(fd);
	return 1;

      bad:
	free(dmi);
	if (fd >= 0)
		close(fd);
	return 0;
}
