/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "libdm-targets.h"
#include "libdm-common.h"
#include "libdm-file.h"

#ifdef DM_COMPAT
#  include "libdm-compat.h"
#endif

#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <limits.h>

#ifdef linux
#  include "kdev_t.h"
#  include <linux/limits.h>
#else
#  define MAJOR(x) major((x))
#  define MINOR(x) minor((x))
#  define MKDEV(x,y) makedev((x),(y))
#endif

#include <linux/dm-ioctl.h>

/*
 * Ensure build compatibility.  
 * The hard-coded versions here are the highest present 
 * in the _cmd_data arrays.
 */

#if !((DM_VERSION_MAJOR == 1 && DM_VERSION_MINOR >= 0) || \
      (DM_VERSION_MAJOR == 4 && DM_VERSION_MINOR >= 0))
#error The version of dm-ioctl.h included is incompatible.
#endif

/* FIXME This should be exported in device-mapper.h */
#define DM_NAME "device-mapper"

#define PROC_MISC "/proc/misc"
#define PROC_DEVICES "/proc/devices"
#define MISC_NAME "misc"

/* dm major version no for running kernel */
static int _dm_version = DM_VERSION_MAJOR;
static int _log_suppress = 0;

static int _control_fd = -1;
static int _version_checked = 0;
static int _version_ok = 1;

/*
 * Support both old and new major numbers to ease the transition.
 * Clumsy, but only temporary.
 */
#if DM_VERSION_MAJOR == 4 && defined(DM_COMPAT)
const int _dm_compat = 1;
#else
const int _dm_compat = 0;
#endif


/* *INDENT-OFF* */
static struct cmd_data _cmd_data_v4[] = {
	{"create",	DM_DEV_CREATE,		{4, 0, 0}},
	{"reload",	DM_TABLE_LOAD,		{4, 0, 0}},
	{"remove",	DM_DEV_REMOVE,		{4, 0, 0}},
	{"remove_all",	DM_REMOVE_ALL,		{4, 0, 0}},
	{"suspend",	DM_DEV_SUSPEND,		{4, 0, 0}},
	{"resume",	DM_DEV_SUSPEND,		{4, 0, 0}},
	{"info",	DM_DEV_STATUS,		{4, 0, 0}},
	{"deps",	DM_TABLE_DEPS,		{4, 0, 0}},
	{"rename",	DM_DEV_RENAME,		{4, 0, 0}},
	{"version",	DM_VERSION,		{4, 0, 0}},
	{"status",	DM_TABLE_STATUS,	{4, 0, 0}},
	{"table",	DM_TABLE_STATUS,	{4, 0, 0}},
	{"waitevent",	DM_DEV_WAIT,		{4, 0, 0}},
	{"names",	DM_LIST_DEVICES,	{4, 0, 0}},
	{"clear",	DM_TABLE_CLEAR,		{4, 0, 0}},
	{"mknodes",	DM_DEV_STATUS,		{4, 0, 0}},
#ifdef DM_LIST_VERSIONS
	{"versions",	DM_LIST_VERSIONS,	{4, 1, 0}},
#endif
#ifdef DM_TARGET_MSG
	{"message",	DM_TARGET_MSG,		{4, 2, 0}},
#endif
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

#ifdef DM_IOCTLS
static int _get_proc_number(const char *file, const char *name,
			    uint32_t *number)
{
	FILE *fl;
	char nm[256];
	int c;

	if (!(fl = fopen(file, "r"))) {
		log_error("%s: fopen failed: %s", file, strerror(errno));
		return 0;
	}

	while (!feof(fl)) {
		if (fscanf(fl, "%d %255s\n", number, &nm[0]) == 2) {
			if (!strcmp(name, nm)) {
				fclose(fl);
				return 1;
			}
		} else do {
			c = fgetc(fl);
		} while (c != EOF && c != '\n');
	}
	fclose(fl);

	log_error("%s: No entry for %s found", file, name);
	return 0;
}

static int _control_device_number(uint32_t *major, uint32_t *minor)
{
	if (!_get_proc_number(PROC_DEVICES, MISC_NAME, major) ||
	    !_get_proc_number(PROC_MISC, DM_NAME, minor)) {
		*major = 0;
		return 0;
	}

	return 1;
}

/*
 * Returns 1 if exists; 0 if it doesn't; -1 if it's wrong
 */
static int _control_exists(const char *control, uint32_t major, uint32_t minor)
{
	struct stat buf;

	if (stat(control, &buf) < 0) {
		if (errno != ENOENT)
			log_error("%s: stat failed: %s", control,
				  strerror(errno));
		return 0;
	}

	if (!S_ISCHR(buf.st_mode)) {
		log_verbose("%s: Wrong inode type", control);
		if (!unlink(control))
			return 0;
		log_error("%s: unlink failed: %s", control,
			  strerror(errno));
		return -1;
	}

	if (major && buf.st_rdev != MKDEV(major, minor)) {
		log_verbose("%s: Wrong device number: (%u, %u) instead of "
			    "(%u, %u)", control,
			    MAJOR(buf.st_mode), MINOR(buf.st_mode),
			    major, minor);
		if (!unlink(control))
			return 0;
		log_error("%s: unlink failed: %s", control,
			  strerror(errno));
		return -1;
	}

	return 1;
}

static int _create_control(const char *control, uint32_t major, uint32_t minor)
{
	int ret;
	mode_t old_umask;

	if (!major)
		return 0;

	old_umask = umask(0022);
	ret = create_dir(dm_dir());
	umask(old_umask);

	if (!ret)
		return 0;

	log_verbose("Creating device %s (%u, %u)", control, major, minor);

	if (mknod(control, S_IFCHR | S_IRUSR | S_IWUSR,
		  MKDEV(major, minor)) < 0)  {
		log_error("%s: mknod failed: %s", control, strerror(errno));
		return 0;
	}

#ifdef HAVE_SELINUX
        if (!set_selinux_context(control, S_IFCHR)) {
                stack;
                return 0;
        }
#endif

	return 1;
}
#endif

static int _open_control(void)
{
#ifdef DM_IOCTLS
	char control[PATH_MAX];
	uint32_t major = 0, minor;

	if (_control_fd != -1)
		return 1;

	snprintf(control, sizeof(control), "%s/control", dm_dir());

	if (!_control_device_number(&major, &minor))
		log_error("Is device-mapper driver missing from kernel?");

	if (!_control_exists(control, major, minor) &&
	    !_create_control(control, major, minor))
		goto error;

	if ((_control_fd = open(control, O_RDWR)) < 0) {
		log_error("%s: open failed: %s", control, strerror(errno));
		goto error;
	}

	return 1;

error:
	log_error("Failure to communicate with kernel device-mapper driver.");
	return 0;
#else
	return 1;
#endif
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

	if (dmt->message)
		free(dmt->message);

	if (dmt->dmi.v4)
		free(dmt->dmi.v4);

	if (dmt->uuid)
		free(dmt->uuid);

	free(dmt);
}

/*
 * Protocol Version 1 compatibility functions.
 */

#ifdef DM_COMPAT

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
					outptr + sizeof(*spec))) {
			return 0;
		}

		outptr = outbuf + spec->next;
	}

	return 1;
}

static int _dm_format_dev_v1(char *buf, int bufsize, uint32_t dev_major,
			     uint32_t dev_minor)
{
	int r;

	if (bufsize < 8)
		return 0;

	r = snprintf(buf, bufsize, "%03x:%03x", dev_major, dev_minor);
	if (r < 0 || r > bufsize - 1)
		return 0;

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
	info->live_table = 1;
	info->inactive_table = 0;

	return 1;
}

static const char *_dm_task_get_name_v1(struct dm_task *dmt)
{
	return (dmt->dmi.v1->name);
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

static struct dm_names *_dm_task_get_names_v1(struct dm_task *dmt)
{
	return (struct dm_names *) (((void *) dmt->dmi.v1) +
				    dmt->dmi.v1->data_start);
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

static int _dm_names_v1(struct dm_ioctl_v1 *dmi)
{
	const char *dev_dir = dm_dir();
	int r = 1, len;
	const char *name;
	struct dirent *dirent;
	DIR *d;
	struct dm_names *names, *old_names = NULL;
	void *end = (void *) dmi + dmi->data_size;
	struct stat buf;
	char path[PATH_MAX];

	log_print("Warning: Device list may be incomplete with interface "
		  "version 1.");
	log_print("Please upgrade your kernel device-mapper driver.");

	if (!(d = opendir(dev_dir))) {
		log_error("%s: opendir failed: %s", dev_dir, strerror(errno));
		return 0;
	}

	names = (struct dm_names *) ((void *) dmi + dmi->data_start);

	names->dev = 0;		/* Flags no data */

	while ((dirent = readdir(d))) {
		name = dirent->d_name;

		if (name[0] == '.' || !strcmp(name, "control"))
			continue;

		if (old_names)
			old_names->next = (uint32_t) ((void *) names -
						      (void *) old_names);
		snprintf(path, sizeof(path), "%s/%s", dev_dir, name);
		if (stat(path, &buf)) {
			log_error("%s: stat failed: %s", path, strerror(errno));
			continue;
		}
		if (!S_ISBLK(buf.st_mode))
			continue;
		names->dev = (uint64_t) buf.st_rdev;
		names->next = 0;
		len = strlen(name);
		if (((void *) (names + 1) + len + 1) >= end) {
			log_error("Insufficient buffer space for device list");
			r = 0;
			break;
		}

		strcpy(names->name, name);

		old_names = names;
		names = _align((void *) ++names + len + 1, ALIGNMENT);
	}

	if (closedir(d))
		log_error("%s: closedir failed: %s", dev_dir, strerror(errno));

	return r;
}

static int _dm_task_run_v1(struct dm_task *dmt)
{
	struct dm_ioctl_v1 *dmi;
	unsigned int command;

	dmi = _flatten_v1(dmt);
	if (!dmi) {
		log_error("Couldn't create ioctl argument");
		return 0;
	}

	if (!_open_control())
		return 0;

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
	if (dmt->type == DM_DEVICE_LIST) {
		if (!_dm_names_v1(dmi))
			goto bad;
	} 
#ifdef DM_IOCTLS
	else if (ioctl(_control_fd, command, dmi) < 0) {
		if (_log_suppress)
			log_verbose("device-mapper ioctl cmd %d failed: %s",
				    _IOC_NR(command), strerror(errno));
		else
			log_error("device-mapper ioctl cmd %d failed: %s",
				  _IOC_NR(command), strerror(errno));
		goto bad;
	}
#else /* Userspace alternative for testing */
#endif

	if (dmi->flags & DM_BUFFER_FULL_FLAG)
		/* FIXME Increase buffer size and retry operation (if query) */
		log_error("Warning: libdevmapper buffer too small for data");

	switch (dmt->type) {
	case DM_DEVICE_CREATE:
		add_dev_node(dmt->dev_name, MAJOR(dmi->dev), MINOR(dmi->dev),
			     dmt->uid, dmt->gid, dmt->mode);
		break;

	case DM_DEVICE_REMOVE:
		rm_dev_node(dmt->dev_name);
		break;

	case DM_DEVICE_RENAME:
		rename_dev_node(dmt->dev_name, dmt->newname);
		break;

	case DM_DEVICE_MKNODES:
		if (dmi->flags & DM_EXISTS_FLAG)
			add_dev_node(dmt->dev_name, MAJOR(dmi->dev),
				     MINOR(dmi->dev),
				     dmt->uid, dmt->gid, dmt->mode);
		else
			rm_dev_node(dmt->dev_name);
		break;

	case DM_DEVICE_STATUS:
	case DM_DEVICE_TABLE:
		if (!_unmarshal_status_v1(dmt, dmi))
			goto bad;
		break;

	case DM_DEVICE_SUSPEND:
	case DM_DEVICE_RESUME:
		dmt->type = DM_DEVICE_INFO;
		if (!dm_task_run(dmt))
			goto bad;
		free(dmi);	/* We'll use what info returned */
		return 1;
	}

	dmt->dmi.v1 = dmi;
	return 1;

      bad:
	free(dmi);
	return 0;
}

#endif

/*
 * Protocol Version 4 functions.
 */

int dm_task_get_driver_version(struct dm_task *dmt, char *version, size_t size)
{
	unsigned int *v;

#ifdef DM_COMPAT
	if (_dm_version == 1)
		return _dm_task_get_driver_version_v1(dmt, version, size);
#endif

	if (!dmt->dmi.v4) {
		version[0] = '\0';
		return 0;
	}

	v = dmt->dmi.v4->version;
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
	char libversion[64], dmversion[64];
	const char *compat = "";

	if (_version_checked)
		return _version_ok;

	_version_checked = 1;

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
	_version_ok = 0;
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
					outptr + sizeof(*spec))) {
			return 0;
		}

		outptr = outbuf + spec->next;
	}

	return 1;
}

int dm_format_dev(char *buf, int bufsize, uint32_t dev_major,
		  uint32_t dev_minor)
{
	int r;

#ifdef DM_COMPAT
	if (_dm_version == 1)
		return _dm_format_dev_v1(buf, bufsize, dev_major, dev_minor);
#endif

	if (bufsize < 8)
		return 0;

	r = snprintf(buf, bufsize, "%03u:%03u", dev_major, dev_minor);
	if (r < 0 || r > bufsize - 1)
		return 0;

	return 1;
}

int dm_task_get_info(struct dm_task *dmt, struct dm_info *info)
{
#ifdef DM_COMPAT
	if (_dm_version == 1)
		return _dm_task_get_info_v1(dmt, info);
#endif

	if (!dmt->dmi.v4)
		return 0;

	memset(info, 0, sizeof(*info));

	info->exists = dmt->dmi.v4->flags & DM_EXISTS_FLAG ? 1 : 0;
	if (!info->exists)
		return 1;

	info->suspended = dmt->dmi.v4->flags & DM_SUSPEND_FLAG ? 1 : 0;
	info->read_only = dmt->dmi.v4->flags & DM_READONLY_FLAG ? 1 : 0;
	info->live_table = dmt->dmi.v4->flags & DM_ACTIVE_PRESENT_FLAG ? 1 : 0;
	info->inactive_table = dmt->dmi.v4->flags & DM_INACTIVE_PRESENT_FLAG ?
	    1 : 0;
	info->target_count = dmt->dmi.v4->target_count;
	info->open_count = dmt->dmi.v4->open_count;
	info->event_nr = dmt->dmi.v4->event_nr;
	info->major = MAJOR(dmt->dmi.v4->dev);
	info->minor = MINOR(dmt->dmi.v4->dev);

	return 1;
}

const char *dm_task_get_name(struct dm_task *dmt)
{
#ifdef DM_COMPAT
	if (_dm_version == 1)
		return _dm_task_get_name_v1(dmt);
#endif

	return (dmt->dmi.v4->name);
}

const char *dm_task_get_uuid(struct dm_task *dmt)
{
#ifdef DM_COMPAT
	if (_dm_version == 1)
		return _dm_task_get_uuid_v1(dmt);
#endif

	return (dmt->dmi.v4->uuid);
}

struct dm_deps *dm_task_get_deps(struct dm_task *dmt)
{
#ifdef DM_COMPAT
	if (_dm_version == 1)
		return _dm_task_get_deps_v1(dmt);
#endif

	return (struct dm_deps *) (((void *) dmt->dmi.v4) +
				   dmt->dmi.v4->data_start);
}

struct dm_names *dm_task_get_names(struct dm_task *dmt)
{
#ifdef DM_COMPAT
	if (_dm_version == 1)
		return _dm_task_get_names_v1(dmt);
#endif

	return (struct dm_names *) (((void *) dmt->dmi.v4) +
				    dmt->dmi.v4->data_start);
}

struct dm_versions *dm_task_get_versions(struct dm_task *dmt)
{
	return (struct dm_versions *) (((void *) dmt->dmi.v4) +
				       dmt->dmi.v4->data_start);
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

int dm_task_set_message(struct dm_task *dmt, const char *message)
{
	if (!(dmt->message = strdup(message))) {
		log_error("dm_task_set_message: strdup(%s) failed", message);
		return 0;
	}

	return 1;
}

int dm_task_set_sector(struct dm_task *dmt, uint64_t sector)
{
	dmt->sector = sector;

	return 1;
}

int dm_task_no_open_count(struct dm_task *dmt)
{
	dmt->no_open_count = 1;

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
	struct dm_target_msg *tmsg;
	size_t len = sizeof(struct dm_ioctl);
	void *b, *e;
	int count = 0;

	for (t = dmt->head; t; t = t->next) {
		len += sizeof(struct dm_target_spec);
		len += strlen(t->params) + 1 + ALIGNMENT;
		count++;
	}

	if (count && (dmt->sector || dmt->message)) {
		log_error("targets and message are incompatible");
		return NULL;
	}

	if (count && dmt->newname) {
		log_error("targets and newname are incompatible");
		return NULL;
	}

	if (dmt->newname && (dmt->sector || dmt->message)) {
		log_error("message and newname are incompatible");
		return NULL;
	}

	if (dmt->sector && !dmt->message) {
		log_error("message is required with sector");
		return NULL;
	}

	if (dmt->newname)
		len += strlen(dmt->newname) + 1;

	if (dmt->message)
		len += sizeof(struct dm_target_msg) + strlen(dmt->message) + 1;

	/*
	 * Give len a minimum size so that we have space to store
	 * dependencies or status information.
	 */
	if (len < min_size)
		len = min_size;

	if (!(dmi = malloc(len)))
		return NULL;

	memset(dmi, 0, len);

	version = &_cmd_data_v4[dmt->type].version;

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

	if (dmt->message) {
		tmsg = (struct dm_target_msg *) b;
		tmsg->sector = dmt->sector;
		strcpy(tmsg->message, dmt->message);
	}

	return dmi;

      bad:
	free(dmi);
	return NULL;
}

static int _process_mapper_dir(struct dm_task *dmt)
{
	struct dirent *dirent;
	DIR *d;
	const char *dir;
	int r = 1;

	dir = dm_dir();
	if (!(d = opendir(dir))) {
		fprintf(stderr, "opendir %s: %s", dir, strerror(errno));
		return 0;
	}

	while ((dirent = readdir(d))) {
		if (!strcmp(dirent->d_name, ".") ||
		    !strcmp(dirent->d_name, "..") ||
		    !strcmp(dirent->d_name, "control"))
			continue;
		dm_task_set_name(dmt, dirent->d_name);
		dm_task_run(dmt);
	}

	if (closedir(d)) {
		fprintf(stderr, "closedir %s: %s", dir, strerror(errno));
	}

	return r;
}

static int _process_all_v4(struct dm_task *dmt)
{
	struct dm_task *task;
	struct dm_names *names;
	unsigned next = 0;
	int r = 1;

	if (!(task = dm_task_create(DM_DEVICE_LIST)))
		return 0;

	if (!dm_task_run(task)) {
		r = 0;
		goto out;
	}

	if (!(names = dm_task_get_names(task))) {
		r = 0;
		goto out;
	}

	if (!names->dev)
		goto out;

	do {
		names = (void *) names + next;
		if (!dm_task_set_name(dmt, names->name)) {
			r = 0;
			goto out;
		}
		if (!dm_task_run(dmt))
			r = 0;
		next = names->next;
	} while (next);

      out:
	dm_task_destroy(task);
	return r;
}

static int _mknodes_v4(struct dm_task *dmt)
{
	(void) _process_mapper_dir(dmt);

	return _process_all_v4(dmt);
}

static int _create_and_load_v4(struct dm_task *dmt)
{
	struct dm_task *task;
	int r;

	/* Use new task struct to create the device */
	if (!(task = dm_task_create(DM_DEVICE_CREATE))) {
		log_error("Failed to create device-mapper task struct");
		return 0;
	}

	/* Copy across relevant fields */
	if (dmt->dev_name && !dm_task_set_name(task, dmt->dev_name)) {
		dm_task_destroy(task);
		return 0;
	}

	if (dmt->uuid && !dm_task_set_uuid(task, dmt->uuid)) {
		dm_task_destroy(task);
		return 0;
	}

	task->major = dmt->major;
	task->minor = dmt->minor;

	r = dm_task_run(task);
	dm_task_destroy(task);
	if (!r)
		return r;

	/* Next load the table */
	if (!(task = dm_task_create(DM_DEVICE_RELOAD))) {
		log_error("Failed to create device-mapper task struct");
		return 0;
	}

	/* Copy across relevant fields */
	if (dmt->dev_name && !dm_task_set_name(task, dmt->dev_name)) {
		dm_task_destroy(task);
		return 0;
	}

	task->read_only = dmt->read_only;
	task->head = dmt->head;
	task->tail = dmt->tail;

	r = dm_task_run(task);

	task->head = NULL;
	task->tail = NULL;
	dm_task_destroy(task);
	if (!r)
		return r;

	/* Use the original structure last so the info will be correct */
	dmt->type = DM_DEVICE_RESUME;
	free(dmt->uuid);
	dmt->uuid = NULL;

	r = dm_task_run(dmt);

	return r;
}

int dm_task_run(struct dm_task *dmt)
{
	struct dm_ioctl *dmi = NULL;
	unsigned int command;

#ifdef DM_COMPAT
	if (_dm_version == 1)
		return _dm_task_run_v1(dmt);
#endif

	if ((unsigned) dmt->type >=
	    (sizeof(_cmd_data_v4) / sizeof(*_cmd_data_v4))) {
		log_error("Internal error: unknown device-mapper task %d",
			  dmt->type);
		goto bad;
	}

	command = _cmd_data_v4[dmt->type].cmd;

	/* Old-style creation had a table supplied */
	if (dmt->type == DM_DEVICE_CREATE && dmt->head)
		return _create_and_load_v4(dmt);

	if (dmt->type == DM_DEVICE_MKNODES && !dmt->dev_name &&
	    !dmt->uuid && dmt->major <= 0)
		return _mknodes_v4(dmt);

	if (!_open_control())
		return 0;

	dmi = _flatten(dmt);
	if (!dmi) {
		log_error("Couldn't create ioctl argument");
		return 0;
	}

	if (dmt->type == DM_DEVICE_TABLE)
		dmi->flags |= DM_STATUS_TABLE_FLAG;

	dmi->flags |= DM_EXISTS_FLAG;	/* FIXME */

	if (dmt->no_open_count)
		dmi->flags |= DM_SKIP_BDGET_FLAG;

	log_debug("dm %s %s %s %s%c %.0llu %s", _cmd_data_v4[dmt->type].name,
		  dmi->name, dmi->uuid, dmt->newname ? dmt->newname : "",
		  dmt->no_open_count ? 'N' : 'O',
		  dmt->sector, dmt->message ? dmt->message : "");
#ifdef DM_IOCTLS
	if (ioctl(_control_fd, command, dmi) < 0) {
		if (errno == ENXIO && ((dmt->type == DM_DEVICE_INFO) ||
				       (dmt->type == DM_DEVICE_MKNODES)))
			dmi->flags &= ~DM_EXISTS_FLAG;	/* FIXME */
		else {
			if (_log_suppress)
				log_verbose("device-mapper ioctl "
					    "cmd %d failed: %s",
					    _IOC_NR(command), strerror(errno));
			else
				log_error("device-mapper ioctl "
					  "cmd %d failed: %s",
					  _IOC_NR(command), strerror(errno));
			goto bad;
		}
	}
#else /* Userspace alternative for testing */
#endif

	switch (dmt->type) {
	case DM_DEVICE_CREATE:
		add_dev_node(dmt->dev_name, MAJOR(dmi->dev), MINOR(dmi->dev),
			     dmt->uid, dmt->gid, dmt->mode);
		break;

	case DM_DEVICE_REMOVE:
		/* FIXME Kernel needs to fill in dmi->name */
		if (dmt->dev_name)
			rm_dev_node(dmt->dev_name);
		break;

	case DM_DEVICE_RENAME:
		/* FIXME Kernel needs to fill in dmi->name */
		if (dmt->dev_name)
			rename_dev_node(dmt->dev_name, dmt->newname);
		break;

	case DM_DEVICE_MKNODES:
		if (dmi->flags & DM_EXISTS_FLAG)
			add_dev_node(dmi->name, MAJOR(dmi->dev),
				     MINOR(dmi->dev),
				     dmt->uid, dmt->gid, dmt->mode);
		else if (dmt->dev_name)
			rm_dev_node(dmt->dev_name);
		break;

	case DM_DEVICE_STATUS:
	case DM_DEVICE_TABLE:
	case DM_DEVICE_WAITEVENT:
		if (!_unmarshal_status(dmt, dmi))
			goto bad;
		break;
	}

	dmt->dmi.v4 = dmi;
	return 1;

      bad:
	free(dmi);
	return 0;
}

void dm_lib_release(void)
{
	if (_control_fd != -1) {
		close(_control_fd);
		_control_fd = -1;
	}
	update_devs();
}

void dm_lib_exit(void)
{
	dm_lib_release();
	_version_ok = 1;
	_version_checked = 0;
}
