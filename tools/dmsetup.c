/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2005 NEC Corperation
 *
 * This file is part of the device-mapper userspace tools.
 *
 * It includes tree drawing code based on pstree: http://psmisc.sourceforge.net/
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <configure.h>

#include "libdevmapper.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/param.h>
#include <locale.h>
#include <langinfo.h>

#include <fcntl.h>
#include <sys/stat.h>

/* FIXME Unused so far */
#undef HAVE_SYS_STATVFS_H

#ifdef HAVE_SYS_STATVFS_H
#  include <sys/statvfs.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#  include <sys/ioctl.h>
#endif

#if HAVE_TERMIOS_H
#  include <termios.h>
#endif

#ifdef HAVE_GETOPTLONG
#  include <getopt.h>
#  define GETOPTLONG_FN(a, b, c, d, e) getopt_long((a), (b), (c), (d), (e))
#  define OPTIND_INIT 0
#else
struct option {
};
extern int optind;
extern char *optarg;
#  define GETOPTLONG_FN(a, b, c, d, e) getopt((a), (b), (c))
#  define OPTIND_INIT 1
#endif

#ifndef TEMP_FAILURE_RETRY
# define TEMP_FAILURE_RETRY(expression) \
  (__extension__                                                              \
    ({ long int __result;                                                     \
       do __result = (long int) (expression);                                 \
       while (__result == -1L && errno == EINTR);                             \
       __result; }))
#endif

#ifdef linux
#  include "kdev_t.h"
#else
#  define MAJOR(x) major((x))
#  define MINOR(x) minor((x))
#  define MKDEV(x,y) makedev((x),(y))
#endif

#define LINE_SIZE 4096
#define ARGS_MAX 256
#define LOOP_TABLE_SIZE (PATH_MAX + 255)

/* FIXME Should be elsewhere */
#define SECTOR_SHIFT 9L
#define DEV_PATH "/dev/"

#define err(msg, x...) fprintf(stderr, msg "\n", ##x)

/*
 * We have only very simple switches ATM.
 */
enum {
	READ_ONLY = 0,
	COLS_ARG,
	EXEC_ARG,
	FORCE_ARG,
	GID_ARG,
	MAJOR_ARG,
	MINOR_ARG,
	MODE_ARG,
	NOFLUSH_ARG,
	NOHEADINGS_ARG,
	NOLOCKFS_ARG,
	NOOPENCOUNT_ARG,
	NOTABLE_ARG,
	OPTIONS_ARG,
	SHOWKEYS_ARG,
	TABLE_ARG,
	TARGET_ARG,
	TREE_ARG,
	UID_ARG,
	UUID_ARG,
	VERBOSE_ARG,
	VERSION_ARG,
	NUM_SWITCHES
};

static int _switches[NUM_SWITCHES];
static int _values[NUM_SWITCHES];
static int _num_devices;
static char *_uuid;
static char *_fields;
static char *_table;
static char *_target;
static char *_command;
static struct dm_tree *_dtree;

/*
 * Commands
 */
static int _parse_line(struct dm_task *dmt, char *buffer, const char *file,
		       int line)
{
	char ttype[LINE_SIZE], *ptr, *comment;
	unsigned long long start, size;
	int n;

	/* trim trailing space */
	for (ptr = buffer + strlen(buffer) - 1; ptr >= buffer; ptr--)
		if (!isspace((int) *ptr))
			break;
	ptr++;
	*ptr = '\0';

	/* trim leading space */
	for (ptr = buffer; *ptr && isspace((int) *ptr); ptr++)
		;

	if (!*ptr || *ptr == '#')
		return 1;

	if (sscanf(ptr, "%llu %llu %s %n",
		   &start, &size, ttype, &n) < 3) {
		err("Invalid format on line %d of table %s", line, file);
		return 0;
	}

	ptr += n;
	if ((comment = strchr(ptr, (int) '#')))
		*comment = '\0';

	if (!dm_task_add_target(dmt, start, size, ttype, ptr))
		return 0;

	return 1;
}

static int _parse_file(struct dm_task *dmt, const char *file)
{
	char *buffer = NULL;
	size_t buffer_size = 0;
	FILE *fp;
	int r = 0, line = 0;

	/* one-line table on cmdline */
	if (_table)
		return _parse_line(dmt, _table, "", ++line);

	/* OK for empty stdin */
	if (file) {
		if (!(fp = fopen(file, "r"))) {
			err("Couldn't open '%s' for reading", file);
			return 0;
		}
	} else
		fp = stdin;

#ifndef HAVE_GETLINE
	buffer_size = LINE_SIZE;
	if (!(buffer = dm_malloc(buffer_size))) {
		err("Failed to malloc line buffer.");
		return 0;
	}

	while (fgets(buffer, (int) buffer_size, fp))
#else
	while (getline(&buffer, &buffer_size, fp) > 0)
#endif
		if (!_parse_line(dmt, buffer, file ? : "on stdin", ++line))
			goto out;

	r = 1;

      out:
#ifndef HAVE_GETLINE
	dm_free(buffer);
#else
	free(buffer);
#endif
	if (file)
		fclose(fp);
	return r;
}

static void _display_info_cols_noheadings(struct dm_task *dmt,
					  struct dm_info *info)
{
	const char *uuid;

	if (!info->exists)
		return;

	uuid = dm_task_get_uuid(dmt);

	if (_switches[OPTIONS_ARG])
		printf("%s\n", dm_task_get_name(dmt));
	else
		printf("%s:%d:%d:%s%s%s%s:%d:%d:%" PRIu32 ":%s\n",
		       dm_task_get_name(dmt),
		       info->major, info->minor,
		       info->live_table ? "L" : "-",
		       info->inactive_table ? "I" : "-",
		       info->suspended ? "s" : "-",
		       info->read_only ? "r" : "w",
		       info->open_count, info->target_count, info->event_nr,
		       uuid && *uuid ? uuid : "");
}

static void _display_info_cols(struct dm_task *dmt, struct dm_info *info)
{
	static int _headings = 0;
	const char *uuid;

	if (!info->exists) {
		printf("Device does not exist.\n");
		return;
	}

	if (!_headings) {
		if (_switches[OPTIONS_ARG])
			printf("Name\n");
		else
			printf("Name             Maj Min Stat Open Targ "
			       "Event  UUID\n");
		_headings = 1;
	}

	if (_switches[OPTIONS_ARG])
		printf("%s\n", dm_task_get_name(dmt));
	else {
		printf("%-16s %3d %3d %s%s%s%s %4d %4d %6" PRIu32 " ",
		       dm_task_get_name(dmt),
		       info->major, info->minor,
		       info->live_table ? "L" : "-",
		       info->inactive_table ? "I" : "-",
		       info->suspended ? "s" : "-",
		       info->read_only ? "r" : "w",
		       info->open_count, info->target_count, info->event_nr);

		if ((uuid = dm_task_get_uuid(dmt)) && *uuid)
			printf("%s", uuid);

		printf("\n");
	}
}

static void _display_info_long(struct dm_task *dmt, struct dm_info *info)
{
	const char *uuid;

	if (!info->exists) {
		printf("Device does not exist.\n");
		return;
	}

	printf("Name:              %s\n", dm_task_get_name(dmt));

	printf("State:             %s%s\n",
	       info->suspended ? "SUSPENDED" : "ACTIVE",
	       info->read_only ? " (READ-ONLY)" : "");

	if (!info->live_table && !info->inactive_table)
		printf("Tables present:    None\n");
	else
		printf("Tables present:    %s%s%s\n",
		       info->live_table ? "LIVE" : "",
		       info->live_table && info->inactive_table ? " & " : "",
		       info->inactive_table ? "INACTIVE" : "");

	if (info->open_count != -1)
		printf("Open count:        %d\n", info->open_count);

	printf("Event number:      %" PRIu32 "\n", info->event_nr);
	printf("Major, minor:      %d, %d\n", info->major, info->minor);

	if (info->target_count != -1)
		printf("Number of targets: %d\n", info->target_count);

	if ((uuid = dm_task_get_uuid(dmt)) && *uuid)
		printf("UUID: %s\n", uuid);

	printf("\n");
}

static int _display_info(struct dm_task *dmt)
{
	struct dm_info info;

	if (!dm_task_get_info(dmt, &info))
		return 0;

	if (!_switches[COLS_ARG])
		_display_info_long(dmt, &info);
	else if (_switches[NOHEADINGS_ARG])
		_display_info_cols_noheadings(dmt, &info);
	else
		_display_info_cols(dmt, &info);

	return info.exists ? 1 : 0;
}

static int _set_task_device(struct dm_task *dmt, const char *name, int optional)
{
	if (name) {
		if (!dm_task_set_name(dmt, name))
			return 0;
	} else if (_switches[UUID_ARG]) {
		if (!dm_task_set_uuid(dmt, _uuid))
			return 0;
	} else if (_switches[MAJOR_ARG] && _switches[MINOR_ARG]) {
		if (!dm_task_set_major(dmt, _values[MAJOR_ARG]) ||
		    !dm_task_set_minor(dmt, _values[MINOR_ARG]))
			return 0;
	} else if (!optional) {
		fprintf(stderr, "No device specified.\n");
		return 0;
	}

	return 1;
}

static int _load(int argc, char **argv, void *data __attribute((unused)))
{
	int r = 0;
	struct dm_task *dmt;
	const char *file = NULL;
	const char *name = NULL;

	if (_switches[NOTABLE_ARG]) {
		err("--notable only available when creating new device\n");
		return 0;
	}

	if (!_switches[UUID_ARG] && !_switches[MAJOR_ARG]) {
		if (argc == 1) {
			err("Please specify device.\n");
			return 0;
		}
		name = argv[1];
		argc--;
		argv++;
	} else if (argc > 2) {
		err("Too many command line arguments.\n");
		return 0;
	}

	if (argc == 2)
		file = argv[1];

	if (!(dmt = dm_task_create(DM_DEVICE_RELOAD)))
		return 0;

	if (!_set_task_device(dmt, name, 0))
		goto out;

	if (!_switches[NOTABLE_ARG] && !_parse_file(dmt, file))
		goto out;

	if (_switches[READ_ONLY] && !dm_task_set_ro(dmt))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	r = 1;

	if (_switches[VERBOSE_ARG])
		r = _display_info(dmt);

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _create(int argc, char **argv, void *data __attribute((unused)))
{
	int r = 0;
	struct dm_task *dmt;
	const char *file = NULL;

	if (argc == 3)
		file = argv[2];

	if (!(dmt = dm_task_create(DM_DEVICE_CREATE)))
		return 0;

	if (!dm_task_set_name(dmt, argv[1]))
		goto out;

	if (_switches[UUID_ARG] && !dm_task_set_uuid(dmt, _uuid))
		goto out;

	if (!_switches[NOTABLE_ARG] && !_parse_file(dmt, file))
		goto out;

	if (_switches[READ_ONLY] && !dm_task_set_ro(dmt))
		goto out;

	if (_switches[MAJOR_ARG] && !dm_task_set_major(dmt, _values[MAJOR_ARG]))
		goto out;

	if (_switches[MINOR_ARG] && !dm_task_set_minor(dmt, _values[MINOR_ARG]))
		goto out;

	if (_switches[UID_ARG] && !dm_task_set_uid(dmt, _values[UID_ARG]))
		goto out;

	if (_switches[GID_ARG] && !dm_task_set_gid(dmt, _values[GID_ARG]))
		goto out;

	if (_switches[MODE_ARG] && !dm_task_set_mode(dmt, _values[MODE_ARG]))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	r = 1;

	if (_switches[VERBOSE_ARG])
		r = _display_info(dmt);

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _rename(int argc, char **argv, void *data __attribute((unused)))
{
	int r = 0;
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_RENAME)))
		return 0;

	/* FIXME Kernel doesn't support uuid or device number here yet */
	if (!_set_task_device(dmt, (argc == 3) ? argv[1] : NULL, 0))
		goto out;

	if (!dm_task_set_newname(dmt, argv[argc - 1]))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	r = 1;

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _message(int argc, char **argv, void *data __attribute((unused)))
{
	int r = 0, i;
	size_t sz = 1;
	struct dm_task *dmt;
	char *str;

	if (!(dmt = dm_task_create(DM_DEVICE_TARGET_MSG)))
		return 0;

	if (_switches[UUID_ARG] || _switches[MAJOR_ARG]) {
		if (!_set_task_device(dmt, NULL, 0))
			goto out;
	} else {
		if (!_set_task_device(dmt, argv[1], 0))
			goto out;
		argc--;
		argv++;
	}

	if (!dm_task_set_sector(dmt, (uint64_t) atoll(argv[1])))
		goto out;

	argc -= 2;
	argv += 2;

	if (argc <= 0)
		err("No message supplied.\n");

	for (i = 0; i < argc; i++)
		sz += strlen(argv[i]) + 1;

	if (!(str = dm_malloc(sz))) {
		err("message string allocation failed");
		goto out;
	}

	memset(str, 0, sz);

	for (i = 0; i < argc; i++) {
		if (i)
			strcat(str, " ");
		strcat(str, argv[i]);
	}

	if (!dm_task_set_message(dmt, str))
		goto out;

	dm_free(str);

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	r = 1;

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _setgeometry(int argc, char **argv, void *data __attribute((unused)))
{
	int r = 0;
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_SET_GEOMETRY)))
		return 0;

	if (_switches[UUID_ARG] || _switches[MAJOR_ARG]) {
		if (!_set_task_device(dmt, NULL, 0))
			goto out;
	} else {
		if (!_set_task_device(dmt, argv[1], 0))
			goto out;
		argc--;
		argv++;
	}

	if (!dm_task_set_geometry(dmt, argv[1], argv[2], argv[3], argv[4]))
		goto out;

	/* run the task */
	if (!dm_task_run(dmt))
		goto out;

	r = 1;

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _version(int argc __attribute((unused)), char **argv __attribute((unused)), void *data __attribute((unused)))
{
	char version[80];

	if (dm_get_library_version(version, sizeof(version)))
		printf("Library version:   %s\n", version);

	if (!dm_driver_version(version, sizeof(version)))
		return 0;

	printf("Driver version:    %s\n", version);

	return 1;
}

static int _simple(int task, const char *name, uint32_t event_nr, int display)
{
	int r = 0;

	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task)))
		return 0;

	if (!_set_task_device(dmt, name, 0))
		goto out;

	if (event_nr && !dm_task_set_event_nr(dmt, event_nr))
		goto out;

	if (_switches[NOFLUSH_ARG] && !dm_task_no_flush(dmt))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (_switches[NOLOCKFS_ARG] && !dm_task_skip_lockfs(dmt))
		goto out;

	r = dm_task_run(dmt);

	if (r && display && _switches[VERBOSE_ARG])
		r = _display_info(dmt);

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _suspend(int argc, char **argv, void *data __attribute((unused)))
{
	return _simple(DM_DEVICE_SUSPEND, argc > 1 ? argv[1] : NULL, 0, 1);
}

static int _resume(int argc, char **argv, void *data __attribute((unused)))
{
	return _simple(DM_DEVICE_RESUME, argc > 1 ? argv[1] : NULL, 0, 1);
}

static int _clear(int argc, char **argv, void *data __attribute((unused)))
{
	return _simple(DM_DEVICE_CLEAR, argc > 1 ? argv[1] : NULL, 0, 1);
}

static int _wait(int argc, char **argv, void *data __attribute((unused)))
{
	const char *name = NULL;

	if (!_switches[UUID_ARG] && !_switches[MAJOR_ARG]) {
		if (argc == 1) {
			err("No device specified.");
			return 0;
		}
		name = argv[1];
		argc--, argv++;
	}

	return _simple(DM_DEVICE_WAITEVENT, name,
		       (argc > 1) ? (uint32_t) atoi(argv[argc - 1]) : 0, 1);
}

static int _process_all(int argc, char **argv, int silent,
			int (*fn) (int argc, char **argv, void *data))
{
	int r = 1;
	struct dm_names *names;
	unsigned next = 0;

	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_LIST)))
		return 0;

	if (!dm_task_run(dmt)) {
		r = 0;
		goto out;
	}

	if (!(names = dm_task_get_names(dmt))) {
		r = 0;
		goto out;
	}

	if (!names->dev) {
		if (!silent)
			printf("No devices found\n");
		goto out;
	}

	do {
		names = (void *) names + next;
		if (!fn(argc, argv, (void *) names))
			r = 0;
		next = names->next;
	} while (next);

      out:
	dm_task_destroy(dmt);
	return r;
}

static uint64_t _get_device_size(const char *name)
{
	uint64_t start, length, size = UINT64_C(0);
	struct dm_info info;
	char *target_type, *params;
	struct dm_task *dmt;
	void *next = NULL;

	if (!(dmt = dm_task_create(DM_DEVICE_TABLE)))
		return 0;

	if (!_set_task_device(dmt, name, 0))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto out;

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);
		size += length;
	} while (next);

      out:
	dm_task_destroy(dmt);
	return size;
}

static int _error_device(int argc __attribute((unused)), char **argv __attribute((unused)), void *data)
{
	struct dm_names *names = (struct dm_names *) data;
	struct dm_task *dmt;
	const char *name;
	uint64_t size;
	int r = 0;

	if (data)
		name = names->name;
	else
		name = argv[1];

	size = _get_device_size(name);

	if (!(dmt = dm_task_create(DM_DEVICE_RELOAD)))
		return 0;

	if (!_set_task_device(dmt, name, 0))
		goto error;

	if (!dm_task_add_target(dmt, 0, size, "error", ""))
		goto error;

	if (_switches[READ_ONLY] && !dm_task_set_ro(dmt))
		goto error;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto error;

	if (!dm_task_run(dmt))
		goto error;

	if (!_simple(DM_DEVICE_RESUME, name, 0, 0)) {
		_simple(DM_DEVICE_CLEAR, name, 0, 0);
		goto error;
	}

	r = 1;

error:
	dm_task_destroy(dmt);
	return r;
}

static int _remove(int argc, char **argv, void *data __attribute((unused)))
{
	int r;

	if (_switches[FORCE_ARG] && argc > 1)
		r = _error_device(argc, argv, NULL);

	return _simple(DM_DEVICE_REMOVE, argc > 1 ? argv[1] : NULL, 0, 0);
}

static int _count_devices(int argc __attribute((unused)), char **argv __attribute((unused)), void *data __attribute((unused)))
{
	_num_devices++;

	return 1;
}

static int _remove_all(int argc __attribute((unused)), char **argv __attribute((unused)), void *data __attribute((unused)))
{
	int r;

	/* Remove all closed devices */
	r =  _simple(DM_DEVICE_REMOVE_ALL, "", 0, 0) | dm_mknodes(NULL);

	if (!_switches[FORCE_ARG])
		return r;

	_num_devices = 0;
	r |= _process_all(argc, argv, 1, _count_devices);

	/* No devices left? */
	if (!_num_devices)
		return r;

	r |= _process_all(argc, argv, 1, _error_device);
	r |= _simple(DM_DEVICE_REMOVE_ALL, "", 0, 0) | dm_mknodes(NULL);

	_num_devices = 0;
	r |= _process_all(argc, argv, 1, _count_devices);
	if (!_num_devices)
		return r;

	fprintf(stderr, "Unable to remove %d device(s).\n", _num_devices);

	return r;
}

static void _display_dev(struct dm_task *dmt, const char *name)
{
	struct dm_info info;

	if (dm_task_get_info(dmt, &info))
		printf("%s\t(%u, %u)\n", name, info.major, info.minor);
}

static int _mknodes(int argc, char **argv, void *data __attribute((unused)))
{
	return dm_mknodes(argc > 1 ? argv[1] : NULL);
}

static int _exec_command(const char *name)
{
	int n;
	static char path[PATH_MAX];
	static char *args[ARGS_MAX + 1];
	static int argc = 0;
	char *c;
	pid_t pid;

	if (argc < 0)
		return 0;

	if (!dm_mknodes(name))
		return 0;

	n = snprintf(path, sizeof(path), "%s/%s", dm_dir(), name);
	if (n < 0 || n > (int) sizeof(path) - 1)
		return 0;

	if (!argc) {
		c = _command;
		while (argc < ARGS_MAX) {
			while (*c && isspace(*c))
				c++;
			if (!*c)
				break;
			args[argc++] = c;
			while (*c && !isspace(*c))
				c++;
			if (*c)
				*c++ = '\0';
		}

		if (!argc) {
			argc = -1;
			return 0;
		}

		if (argc == ARGS_MAX) {
			err("Too many args to --exec\n");
			argc = -1;
			return 0;
		}

		args[argc++] = path;
		args[argc] = NULL;
	}

	if (!(pid = fork())) {
		execvp(args[0], args);
		exit(127);
	} else if (pid < (pid_t) 0)
		return 0;

	TEMP_FAILURE_RETRY(waitpid(pid, NULL, 0));

	return 1;
}

static int _status(int argc, char **argv, void *data)
{
	int r = 0;
	struct dm_task *dmt;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params, *c;
	int cmd;
	struct dm_names *names = (struct dm_names *) data;
	const char *name = NULL;
	int matched = 0;
	int ls_only = 0;
	struct dm_info info;

	if (data)
		name = names->name;
	else {
		if (argc == 1 && !_switches[UUID_ARG] && !_switches[MAJOR_ARG])
			return _process_all(argc, argv, 0, _status);
		if (argc == 2)
			name = argv[1];
	}

	if (!strcmp(argv[0], "table"))
		cmd = DM_DEVICE_TABLE;
	else
		cmd = DM_DEVICE_STATUS;

	if (!strcmp(argv[0], "ls"))
		ls_only = 1;

	if (!(dmt = dm_task_create(cmd)))
		return 0;

	if (!_set_task_device(dmt, name, 0))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto out;

	if (!name)
		name = dm_task_get_name(dmt);

	/* Fetch targets and print 'em */
	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);
		/* Skip if target type doesn't match */
		if (_switches[TARGET_ARG] &&
		    (!target_type || strcmp(target_type, _target)))
			continue;
		if (ls_only) {
			if (!_switches[EXEC_ARG] || !_command ||
			    _switches[VERBOSE_ARG])
				_display_dev(dmt, name);
			next = NULL;
		} else if (!_switches[EXEC_ARG] || !_command ||
			   _switches[VERBOSE_ARG]) {
			if (!matched && _switches[VERBOSE_ARG])
				_display_info(dmt);
			if (data && !_switches[VERBOSE_ARG])
				printf("%s: ", name);
			if (target_type) {

			/* Suppress encryption key */
			if (!_switches[SHOWKEYS_ARG] &&
			    !strcmp(target_type, "crypt")) {
				c = params;
				while (*c && *c != ' ')
					c++;
				c++;
				while (*c && *c != ' ')
					*c++ = '0';
			}
				printf("%" PRIu64 " %" PRIu64 " %s %s",
				       start, length, target_type, params);
			}
			printf("\n");
		}
		matched = 1;
	} while (next);

	if (data && _switches[VERBOSE_ARG] && matched && !ls_only)
		printf("\n");

	if (matched && _switches[EXEC_ARG] && _command && !_exec_command(name))
		goto out;

	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;
}

/* Show target names and their version numbers */
static int _targets(int argc __attribute((unused)), char **argv __attribute((unused)), void *data __attribute((unused)))
{
	int r = 0;
	struct dm_task *dmt;
	struct dm_versions *target;
	struct dm_versions *last_target;

	if (!(dmt = dm_task_create(DM_DEVICE_LIST_VERSIONS)))
		return 0;

	if (!dm_task_run(dmt))
		goto out;

	target = dm_task_get_versions(dmt);

	/* Fetch targets and print 'em */
	do {
		last_target = target;

		printf("%-16s v%d.%d.%d\n", target->name, target->version[0],
		       target->version[1], target->version[2]);

		target = (void *) target + target->next;
	} while (last_target != target);

	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _info(int argc, char **argv, void *data)
{
	int r = 0;

	struct dm_task *dmt;
	struct dm_names *names = (struct dm_names *) data;
	char *name = NULL;

	if (data)
		name = names->name;
	else {
		if (argc == 1 && !_switches[UUID_ARG] && !_switches[MAJOR_ARG])
			return _process_all(argc, argv, 0, _info);
		if (argc == 2)
			name = argv[1];
	}

	if (!(dmt = dm_task_create(DM_DEVICE_INFO)))
		return 0;

	if (!_set_task_device(dmt, name, 0))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	r = _display_info(dmt);

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _deps(int argc, char **argv, void *data)
{
	int r = 0;
	uint32_t i;
	struct dm_deps *deps;
	struct dm_task *dmt;
	struct dm_info info;
	struct dm_names *names = (struct dm_names *) data;
	char *name = NULL;

	if (data)
		name = names->name;
	else {
		if (argc == 1 && !_switches[UUID_ARG] && !_switches[MAJOR_ARG])
			return _process_all(argc, argv, 0, _deps);
		if (argc == 2)
			name = argv[1];
	}

	if (!(dmt = dm_task_create(DM_DEVICE_DEPS)))
		return 0;

	if (!_set_task_device(dmt, name, 0))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &info))
		goto out;

	if (!(deps = dm_task_get_deps(dmt)))
		goto out;

	if (!info.exists) {
		printf("Device does not exist.\n");
		r = 1;
		goto out;
	}

	if (_switches[VERBOSE_ARG])
		_display_info(dmt);

	if (data && !_switches[VERBOSE_ARG])
		printf("%s: ", name);
	printf("%d dependencies\t:", deps->count);

	for (i = 0; i < deps->count; i++)
		printf(" (%d, %d)",
		       (int) MAJOR(deps->device[i]),
		       (int) MINOR(deps->device[i]));
	printf("\n");

	if (data && _switches[VERBOSE_ARG])
		printf("\n");

	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _display_name(int argc __attribute((unused)), char **argv __attribute((unused)), void *data)
{
	struct dm_names *names = (struct dm_names *) data;

	printf("%s\t(%d, %d)\n", names->name,
	       (int) MAJOR(names->dev), (int) MINOR(names->dev));

	return 1;
}

/*
 * Tree drawing code
 */

enum {
	TR_DEVICE=0,	/* display device major:minor number */
	TR_TABLE,
	TR_STATUS,
	TR_ACTIVE,
	TR_RW,
	TR_OPENCOUNT,
	TR_UUID,
	TR_COMPACT,
	TR_TRUNCATE,
	TR_BOTTOMUP,
	NUM_TREEMODE,
};

static int _tree_switches[NUM_TREEMODE];

#define TR_PRINT_ATTRIBUTE ( _tree_switches[TR_ACTIVE] || \
			     _tree_switches[TR_RW] || \
			     _tree_switches[TR_OPENCOUNT] || \
			     _tree_switches[TR_UUID] )

#define TR_PRINT_TARGETS ( _tree_switches[TR_TABLE] || \
			   _tree_switches[TR_STATUS] )

/* Compact - fewer newlines */
#define TR_PRINT_COMPACT (_tree_switches[TR_COMPACT] && \
			  !TR_PRINT_ATTRIBUTE && \
			  !TR_PRINT_TARGETS)

/* FIXME Get rid of this */
#define MAX_DEPTH 100

/* Drawing character definition from pstree */
/* [pstree comment] UTF-8 defines by Johan Myreen, updated by Ben Winslow */
#define UTF_V	"\342\224\202"	/* U+2502, Vertical line drawing char */
#define UTF_VR	"\342\224\234"	/* U+251C, Vertical and right */
#define UTF_H	"\342\224\200"	/* U+2500, Horizontal */
#define UTF_UR	"\342\224\224"	/* U+2514, Up and right */
#define UTF_HD	"\342\224\254"	/* U+252C, Horizontal and down */

#define VT_BEG	"\033(0\017"	/* use graphic chars */
#define VT_END	"\033(B"	/* back to normal char set */
#define VT_V	"x"		/* see UTF definitions above */
#define VT_VR	"t"
#define VT_H	"q"
#define VT_UR	"m"
#define VT_HD	"w"

static struct {
	const char *empty_2;	/*    */
	const char *branch_2;	/* |- */
	const char *vert_2;	/* |  */
	const char *last_2;	/* `- */
	const char *single_3;	/* --- */
	const char *first_3;	/* -+- */
}
_tsym_ascii = {
	"  ",
	"|-",
	"| ",
	"`-",
	"---",
	"-+-"
},
_tsym_utf = {
	"  ",
	UTF_VR UTF_H,
	UTF_V " ",
	UTF_UR UTF_H,
	UTF_H UTF_H UTF_H,
	UTF_H UTF_HD UTF_H
},
_tsym_vt100 = {
	"  ",
	VT_BEG VT_VR VT_H VT_END,
	VT_BEG VT_V VT_END " ",
	VT_BEG VT_UR VT_H VT_END,
	VT_BEG VT_H VT_H VT_H VT_END,
	VT_BEG VT_H VT_HD VT_H VT_END
},
*_tsym = &_tsym_ascii;

/*
 * Tree drawing functions.
 */
/* FIXME Get rid of these statics - use dynamic struct */
/* FIXME Explain what these vars are for */
static int _tree_width[MAX_DEPTH], _tree_more[MAX_DEPTH];
static int _termwidth = 80;	/* Maximum output width */
static int _cur_x = 1;		/* Current horizontal output position */
static char _last_char = 0;

static void _out_char(const unsigned c)
{
	/* Only first UTF-8 char counts */
	_cur_x += ((c & 0xc0) != 0x80);

	if (!_tree_switches[TR_TRUNCATE]) {
		putchar((int) c);
		return;
	}

	/* Truncation? */
	if (_cur_x <= _termwidth)
		putchar((int) c);

	if (_cur_x == _termwidth + 1 && ((c & 0xc0) != 0x80)) {
		if (_last_char || (c & 0x80)) {
			putchar('.');
			putchar('.');
			putchar('.');
		} else {
			_last_char = c;
			_cur_x--;
		}
	}
}

static void _out_string(const unsigned char *str)
{
	while (*str)
		_out_char(*str++);
}

/* non-negative integers only */
static unsigned _out_int(unsigned num)
{
	unsigned digits = 0;
	unsigned divi;

	if (!num) {
		_out_char('0');
		return 1;
	}

	/* non zero case */
	for (divi = 1; num / divi; divi *= 10)
		digits++;

	for (divi /= 10; divi; divi /= 10)
		_out_char('0' + (num / divi) % 10);

	return digits;
}

static void _out_newline(void)
{
	if (_last_char && _cur_x == _termwidth)
		putchar(_last_char);
	_last_char = 0;
	putchar('\n');
	_cur_x = 1;
}

static void _out_prefix(unsigned depth)
{
	unsigned x, d;

	for (d = 0; d < depth; d++) {
		for (x = _tree_width[d] + 1; x > 0; x--)
			_out_char(' ');

		_out_string(d == depth - 1 ?
				!_tree_more[depth] ? _tsym->last_2 : _tsym->branch_2
			   : _tree_more[d + 1] ?
				_tsym->vert_2 : _tsym->empty_2);
	}
}

/*
 * Display tree
 */
static void _display_tree_attributes(struct dm_tree_node *node)
{
	int attr = 0;
	const char *uuid;
	const struct dm_info *info;

	uuid = dm_tree_node_get_uuid(node);
	info = dm_tree_node_get_info(node);

	if (!info->exists)
		return;

	if (_tree_switches[TR_ACTIVE]) {
		_out_string(attr++ ? ", " : " [");
		_out_string(info->suspended ? "SUSPENDED" : "ACTIVE");
	}

	if (_tree_switches[TR_RW]) {
		_out_string(attr++ ? ", " : " [");
		_out_string(info->read_only ? "RO" : "RW");
	}

	if (_tree_switches[TR_OPENCOUNT]) {
		_out_string(attr++ ? ", " : " [");
		(void) _out_int((unsigned) info->open_count);
	}

	if (_tree_switches[TR_UUID]) {
		_out_string(attr++ ? ", " : " [");
		_out_string(uuid && *uuid ? uuid : "");
	}

	if (attr)
		_out_char(']');
}

static void _display_tree_node(struct dm_tree_node *node, unsigned depth,
			       unsigned first_child __attribute((unused)),
			       unsigned last_child, unsigned has_children)
{
	int offset;
	const char *name;
	const struct dm_info *info;
	int first_on_line = 0;

	/* Sub-tree for targets has 2 more depth */
	if (depth + 2 > MAX_DEPTH)
		return;

	name = dm_tree_node_get_name(node);

	if ((!name || !*name) && !_tree_switches[TR_DEVICE])
		return;

	/* Indicate whether there are more nodes at this depth */
	_tree_more[depth] = !last_child;
	_tree_width[depth] = 0;

	if (_cur_x == 1)
		first_on_line = 1;

	if (!TR_PRINT_COMPACT || first_on_line)
		_out_prefix(depth);

	/* Remember the starting point for compact */
	offset = _cur_x;

	if (TR_PRINT_COMPACT && !first_on_line)
		_out_string(_tree_more[depth] ? _tsym->first_3 : _tsym->single_3);

	/* display node */
	if (name)
		_out_string(name);

	info = dm_tree_node_get_info(node);

	if (_tree_switches[TR_DEVICE]) {
		_out_string(name ? " (" : "(");
		(void) _out_int(info->major);
		_out_char(':');
		(void) _out_int(info->minor);
		_out_char(')');
	}

	/* display additional info */
	if (TR_PRINT_ATTRIBUTE)
		_display_tree_attributes(node);

	if (TR_PRINT_COMPACT)
		_tree_width[depth] = _cur_x - offset;

	if (!TR_PRINT_COMPACT || !has_children)
		_out_newline();

	if (TR_PRINT_TARGETS) {
		_tree_more[depth + 1] = has_children;
		// FIXME _display_tree_targets(name, depth + 2);
	}
}

/*
 * Walk the dependency tree
 */
static void _tree_walk_children(struct dm_tree_node *node, unsigned depth)
{
	struct dm_tree_node *child, *next_child;
	void *handle = NULL;
	uint32_t inverted = _tree_switches[TR_BOTTOMUP];
	unsigned first_child = 1;
	unsigned has_children;

	next_child = dm_tree_next_child(&handle, node, inverted);

	while ((child = next_child)) {
		next_child = dm_tree_next_child(&handle, node, inverted);
		has_children =
		    dm_tree_node_num_children(child, inverted) ? 1 : 0;

		_display_tree_node(child, depth, first_child,
				   next_child ? 0U : 1U, has_children);

		if (has_children)
			_tree_walk_children(child, depth + 1);

		first_child = 0;
	}
}

static int _add_dep(int argc __attribute((unused)), char **argv __attribute((unused)), void *data)
{
	struct dm_names *names = (struct dm_names *) data;

	if (!dm_tree_add_dev(_dtree, (unsigned) MAJOR(names->dev), (unsigned) MINOR(names->dev)))
		return 0;

	return 1;
}

/*
 * Create and walk dependency tree
 */
static int _tree(int argc, char **argv, void *data __attribute((unused)))
{
	if (!(_dtree = dm_tree_create()))
		return 0;

	if (!_process_all(argc, argv, 0, _add_dep))
		return 0;

	_tree_walk_children(dm_tree_find_node(_dtree, 0, 0), 0);

	dm_tree_free(_dtree);

	return 1;
}

/*
 * List devices
 */
static int _ls(int argc, char **argv, void *data)
{
	if ((_switches[TARGET_ARG] && _target) ||
	    (_switches[EXEC_ARG] && _command))
		return _status(argc, argv, data);
	else if ((_switches[TREE_ARG]))
		return _tree(argc, argv, data);
	else
		return _process_all(argc, argv, 0, _display_name);
}

/*
 * dispatch table
 */
typedef int (*command_fn) (int argc, char **argv, void *data);

struct command {
	const char *name;
	const char *help;
	int min_args;
	int max_args;
	command_fn fn;
};

static struct command _commands[] = {
	{"create", "<dev_name> [-j|--major <major> -m|--minor <minor>]\n"
	  "\t                  [-U|--uid <uid>] [-G|--gid <gid>] [-M|--mode <octal_mode>]\n"
	  "\t                  [-u|uuid <uuid>]\n"
	  "\t                  [--notable | --table <table> | <table_file>]",
	 1, 2, _create},
	{"remove", "[-f|--force] <device>", 0, 1, _remove},
	{"remove_all", "[-f|--force]", 0, 0, _remove_all},
	{"suspend", "[--noflush] <device>", 0, 1, _suspend},
	{"resume", "<device>", 0, 1, _resume},
	{"load", "<device> [<table_file>]", 0, 2, _load},
	{"clear", "<device>", 0, 1, _clear},
	{"reload", "<device> [<table_file>]", 0, 2, _load},
	{"rename", "<device> <new_name>", 1, 2, _rename},
	{"message", "<device> <sector> <message>", 2, -1, _message},
	{"ls", "[--target <target_type>] [--exec <command>] [--tree [-o options]]", 0, 0, _ls},
	{"info", "[<device>]", 0, 1, _info},
	{"deps", "[<device>]", 0, 1, _deps},
	{"status", "[<device>] [--target <target_type>]", 0, 1, _status},
	{"table", "[<device>] [--target <target_type>] [--showkeys]", 0, 1, _status},
	{"wait", "<device> [<event_nr>]", 0, 2, _wait},
	{"mknodes", "[<device>]", 0, 1, _mknodes},
	{"targets", "", 0, 0, _targets},
	{"version", "", 0, 0, _version},
	{"setgeometry", "<device> <cyl> <head> <sect> <start>", 5, 5, _setgeometry},
	{NULL, NULL, 0, 0, NULL}
};

static void _usage(FILE *out)
{
	int i;

	fprintf(out, "Usage:\n\n");
	fprintf(out, "dmsetup [--version] [-v|--verbose [-v|--verbose ...]]\n"
		"        [-r|--readonly] [--noopencount] [--nolockfs]\n\n");
	for (i = 0; _commands[i].name; i++)
		fprintf(out, "\t%s %s\n", _commands[i].name, _commands[i].help);
	fprintf(out, "\n<device> may be device name or -u <uuid> or "
		     "-j <major> -m <minor>\n");
	fprintf(out, "Table_file contents may be supplied on stdin.\n");
	fprintf(out, "Tree options are: ascii, utf, vt100; compact, inverted, notrunc;\n"
		     "                  [no]device, active, open, rw and uuid.\n\n");
	return;
}

static void _losetup_usage(FILE *out)
{
	fprintf(out, "Usage:\n\n");
	fprintf(out, "losetup [-d|-a] [-e encryption] "
		     "[-o offset] [-f|loop_device] [file]\n\n");
}

static struct command *_find_command(const char *name)
{
	int i;

	for (i = 0; _commands[i].name; i++)
		if (!strcmp(_commands[i].name, name))
			return _commands + i;

	return NULL;
}

static int _process_tree_options(const char *options)
{
	const char *s, *end;
	struct winsize winsz;
	int len;

	/* Symbol set default */
	if (!strcmp(nl_langinfo(CODESET), "UTF-8"))
		_tsym = &_tsym_utf;
	else
		_tsym = &_tsym_ascii;

	/* Default */
	_tree_switches[TR_DEVICE] = 1;
	_tree_switches[TR_TRUNCATE] = 1;

	/* parse */
	for (s = options; s && *s; s++) {
		len = 0;
		for (end = s; *end && *end != ','; end++, len++)
			;
		if (!strncmp(s, "device", len))
			_tree_switches[TR_DEVICE] = 1;
		else if (!strncmp(s, "nodevice", len))
			_tree_switches[TR_DEVICE] = 0;
		else if (!strncmp(s, "status", len))
			_tree_switches[TR_STATUS] = 1;
		else if (!strncmp(s, "table", len))
			_tree_switches[TR_TABLE] = 1;
		else if (!strncmp(s, "active", len))
			_tree_switches[TR_ACTIVE] = 1;
		else if (!strncmp(s, "open", len))
			_tree_switches[TR_OPENCOUNT] = 1;
		else if (!strncmp(s, "uuid", len))
			_tree_switches[TR_UUID] = 1;
		else if (!strncmp(s, "rw", len))
			_tree_switches[TR_RW] = 1;
		else if (!strncmp(s, "utf", len))
			_tsym = &_tsym_utf;
		else if (!strncmp(s, "vt100", len))
			_tsym = &_tsym_vt100;
		else if (!strncmp(s, "ascii", len))
			_tsym = &_tsym_ascii;
		else if (!strncmp(s, "inverted", len))
			_tree_switches[TR_BOTTOMUP] = 1;
		else if (!strncmp(s, "compact", len))
			_tree_switches[TR_COMPACT] = 1;
		else if (!strncmp(s, "notrunc", len))
			_tree_switches[TR_TRUNCATE] = 0;
		else {
			fprintf(stderr, "Tree options not recognised: %s\n", s);
			return 0;
		}
		if (!*end)
			break;
		s = end;
	}

	/* Truncation doesn't work well with vt100 drawing char */
	if (_tsym != &_tsym_vt100)
		if (ioctl(1, TIOCGWINSZ, &winsz) >= 0 && winsz.ws_col > 3)
			_termwidth = winsz.ws_col - 3;

	return 1;
}

/*
 * Returns the full absolute path, or NULL if the path could
 * not be resolved.
 */
static char *_get_abspath(char *path)
{
	char *_path;

#ifdef HAVE_CANONICALIZE_FILE_NAME
	_path = canonicalize_file_name(path);
#else
	/* FIXME Provide alternative */
#endif
	return _path;
}

static char *parse_loop_device_name(char *dev)
{
	char *buf;
	char *device;

	if (!(buf = dm_malloc(PATH_MAX)));
		return NULL;

	if (dev[0] == '/') {
		if (!(device = _get_abspath(dev)))
			goto error;

		if (strncmp(device, DEV_PATH, strlen(DEV_PATH)))
			goto error;

		strncpy(buf, strrchr(device, '/') + 1, PATH_MAX);
		dm_free(device);

	} else {
		/* check for device number */
		if (!strncmp(dev, "loop", strlen("loop")))
			strncpy(buf, dev, PATH_MAX);
		else
			goto error;
	}

	return buf;

error:
	return NULL;
}

/*
 *  create a table for a mapped device using the loop target.
 */
static int _loop_table(char *table, size_t tlen, char *file, char *dev, off_t off)
{
	struct stat fbuf;
	off_t size, sectors;
	int fd = -1;
#ifdef HAVE_SYS_STATVFS_H
	struct statvfs fsbuf;
	off_t blksize;
#endif

	if (!_switches[READ_ONLY])
		fd = open(file, O_RDWR);

	if (fd < 0) {
		_switches[READ_ONLY]++;
		fd = open(file, O_RDONLY);
	}

	if (fd < 0)
		goto error;

	if (fstat(fd, &fbuf))
		goto error;

	size = (fbuf.st_size - off);
	sectors = size >> SECTOR_SHIFT;

	if (_switches[VERBOSE_ARG])
		fprintf(stderr, "losetup: set loop size to %llukB (%llu sectors)\n",
			sectors >> 1, sectors);

#ifdef HAVE_SYS_STATVFS_H
	if (fstatvfs(fd, &fsbuf))
		goto error;       

	/* FIXME Fragment size currently unused */
	blksize = fsbuf.f_frsize;
#endif

	close(fd);

	if (dm_snprintf(table, tlen, "%llu %llu loop %s %llu\n", 0ULL, 
			(long long unsigned)sectors, file, off) < 0)
		return 0;

	if (_switches[VERBOSE_ARG] > 1)
		fprintf(stderr, "Table: %s\n", table);

	return 1;

error:
	if (fd > -1)
		close(fd);
	return 0;
}

static int _process_losetup_switches(const char *base, int *argc, char ***argv)
{
	static int ind;
	int c;
	int encrypt_loop = 0, delete = 0, find = 0, show_all = 0;
	char *device_name = NULL;
	char *loop_file = NULL;
	off_t offset = 0;

#ifdef HAVE_GETOPTLONG
	static struct option long_options[] = {
		{0, 0, 0, 0}
	};
#endif

	optarg = 0;
	optind = OPTIND_INIT;
	while ((ind = -1, c = GETOPTLONG_FN(*argc, *argv, "ade:fo:v",
					    long_options, NULL)) != -1 ) {
		if (c == ':' || c == '?')
			return 0;
		if (c == 'a')
			show_all++;
		if (c == 'd')
			delete++;
		if (c == 'e')
			encrypt_loop++;
		if (c == 'f')
			find++;
		if (c == 'o')
			offset = atoi(optarg);
		if (c == 'v')
			_switches[VERBOSE_ARG]++;
	}

	*argv += optind ;
	*argc -= optind ;

	if (encrypt_loop){
		fprintf(stderr, "%s: Sorry, cryptoloop is not yet implemented "
				"in this version.\n", base);
		return 0;
	}

	if (show_all) {
		fprintf(stderr, "%s: Sorry, show all is not yet implemented "
				"in this version.\n", base);
		return 0;
	}

	if (find) {
		fprintf(stderr, "%s: Sorry, find is not yet implemented "
				"in this version.\n", base);
		if (!*argc)
			return 0;
	}

	if (!*argc) {
		fprintf(stderr, "%s: Please specify loop_device.\n", base);
		_losetup_usage(stderr);
		return 0;
	}

	if (!(device_name = parse_loop_device_name((*argv)[0]))) {
		fprintf(stderr, "%s: Could not parse loop_device %s\n",
			base, (*argv)[0]);
		_losetup_usage(stderr);
		return 0;
	}

	if (delete) {
		*argc = 2;

		(*argv)[1] = device_name;
		(*argv)[0] = (char *) "remove";

		return 1;
	}

	if (*argc != 2) {
		fprintf(stderr, "%s: Too few arguments\n", base);
		_losetup_usage(stderr);
		return 0;
	}

	/* FIXME move these to make them available to native dmsetup */
	if (!(loop_file = _get_abspath((*argv)[(find) ? 0 : 1]))) {
		fprintf(stderr, "%s: Could not parse loop file name %s\n",
			base, (*argv)[1]);
		_losetup_usage(stderr);
		return 0;
	}

	/* FIXME Missing free */
	_table = dm_malloc(LOOP_TABLE_SIZE);
	if (!_loop_table(_table, LOOP_TABLE_SIZE, loop_file, device_name, offset)) {
		fprintf(stderr, "Could not build device-mapper table for %s\n", (*argv)[0]);
		return 0;
	}
	_switches[TABLE_ARG]++;

	(*argv)[0] = (char *) "create";
	(*argv)[1] = device_name ;

	return 1;
}

static int _process_switches(int *argc, char ***argv)
{
	char *base, *namebase;
	static int ind;
	int c, r;

#ifdef HAVE_GETOPTLONG
	static struct option long_options[] = {
		{"readonly", 0, &ind, READ_ONLY},
		{"columns", 0, &ind, COLS_ARG},
		{"exec", 1, &ind, EXEC_ARG},
		{"force", 0, &ind, FORCE_ARG},
		{"gid", 1, &ind, GID_ARG},
		{"major", 1, &ind, MAJOR_ARG},
		{"minor", 1, &ind, MINOR_ARG},
		{"mode", 1, &ind, MODE_ARG},
		{"noflush", 0, &ind, NOFLUSH_ARG},
		{"noheadings", 0, &ind, NOHEADINGS_ARG},
		{"nolockfs", 0, &ind, NOLOCKFS_ARG},
		{"noopencount", 0, &ind, NOOPENCOUNT_ARG},
		{"notable", 0, &ind, NOTABLE_ARG},
		{"options", 1, &ind, OPTIONS_ARG},
		{"showkeys", 0, &ind, SHOWKEYS_ARG},
		{"table", 1, &ind, TABLE_ARG},
		{"target", 1, &ind, TARGET_ARG},
		{"tree", 0, &ind, TREE_ARG},
		{"uid", 1, &ind, UID_ARG},
		{"uuid", 1, &ind, UUID_ARG},
		{"verbose", 1, &ind, VERBOSE_ARG},
		{"version", 0, &ind, VERSION_ARG},
		{0, 0, 0, 0}
	};
#else
	struct option long_options;
#endif

	/*
	 * Zero all the index counts.
	 */
	memset(&_switches, 0, sizeof(_switches));
	memset(&_values, 0, sizeof(_values));

	namebase = strdup((*argv)[0]);
	base = basename(namebase);

	if (!strcmp(base, "devmap_name")) {
		free(namebase);
		_switches[COLS_ARG]++;
		_switches[NOHEADINGS_ARG]++;
		_switches[OPTIONS_ARG]++;
		_switches[MAJOR_ARG]++;
		_switches[MINOR_ARG]++;
		_fields = (char *) "name";

		if (*argc == 3) {
			_values[MAJOR_ARG] = atoi((*argv)[1]);
			_values[MINOR_ARG] = atoi((*argv)[2]);
			*argc -= 2;
			*argv += 2;
		} else if ((*argc == 2) &&
			   (2 == sscanf((*argv)[1], "%i:%i",
					&_values[MAJOR_ARG],
					&_values[MINOR_ARG]))) {
			*argc -= 1;
			*argv += 1;
		} else {
			fprintf(stderr, "Usage: devmap_name <major> <minor>\n");
			return 0;
		}

		(*argv)[0] = (char *) "info";
		return 1;
	}

	if(!strcmp(base, "losetup") || !strcmp(base, "dmlosetup")){
		r = _process_losetup_switches(base, argc, argv);
		free(namebase);
		return r;
	}

	free(namebase);

	optarg = 0;
	optind = OPTIND_INIT;
	while ((ind = -1, c = GETOPTLONG_FN(*argc, *argv, "cCfGj:m:Mno:ru:Uv",
					    long_options, NULL)) != -1) {
		if (c == ':' || c == '?')
			return 0;
		if (c == 'c' || c == 'C' || ind == COLS_ARG)
			_switches[COLS_ARG]++;
		if (c == 'f' || ind == FORCE_ARG)
			_switches[FORCE_ARG]++;
		if (c == 'r' || ind == READ_ONLY)
			_switches[READ_ONLY]++;
		if (c == 'j' || ind == MAJOR_ARG) {
			_switches[MAJOR_ARG]++;
			_values[MAJOR_ARG] = atoi(optarg);
		}
		if (c == 'm' || ind == MINOR_ARG) {
			_switches[MINOR_ARG]++;
			_values[MINOR_ARG] = atoi(optarg);
		}
		if (c == 'n' || ind == NOTABLE_ARG)
			_switches[NOTABLE_ARG]++;
		if (c == 'o' || ind == OPTIONS_ARG) {
			_switches[OPTIONS_ARG]++;
			_fields = optarg;
		}
		if (c == 'v' || ind == VERBOSE_ARG)
			_switches[VERBOSE_ARG]++;
		if (c == 'u' || ind == UUID_ARG) {
			_switches[UUID_ARG]++;
			_uuid = optarg;
		}
		if (c == 'G' || ind == GID_ARG) {
			_switches[GID_ARG]++;
			_values[GID_ARG] = atoi(optarg);
		}
		if (c == 'U' || ind == UID_ARG) {
			_switches[UID_ARG]++;
			_values[UID_ARG] = atoi(optarg);
		}
		if (c == 'M' || ind == MODE_ARG) {
			_switches[MODE_ARG]++;
			/* FIXME Accept modes as per chmod */
			_values[MODE_ARG] = (int) strtol(optarg, NULL, 8);
		}
		if ((ind == EXEC_ARG)) {
			_switches[EXEC_ARG]++;
			_command = optarg;
		}
		if ((ind == TARGET_ARG)) {
			_switches[TARGET_ARG]++;
			_target = optarg;
		}
		if ((ind == NOFLUSH_ARG))
			_switches[NOFLUSH_ARG]++;
		if ((ind == NOHEADINGS_ARG))
			_switches[NOHEADINGS_ARG]++;
		if ((ind == NOLOCKFS_ARG))
			_switches[NOLOCKFS_ARG]++;
		if ((ind == NOOPENCOUNT_ARG))
			_switches[NOOPENCOUNT_ARG]++;
		if ((ind == SHOWKEYS_ARG))
			_switches[SHOWKEYS_ARG]++;
		if ((ind == TABLE_ARG)) {
			_switches[TABLE_ARG]++;
			_table = optarg;
		}
		if ((ind == TREE_ARG))
			_switches[TREE_ARG]++;
		if ((ind == VERSION_ARG))
			_switches[VERSION_ARG]++;
	}

	if (_switches[VERBOSE_ARG] > 1)
		dm_log_init_verbose(_switches[VERBOSE_ARG] - 1);

	if ((_switches[MAJOR_ARG] && !_switches[MINOR_ARG]) ||
	    (!_switches[MAJOR_ARG] && _switches[MINOR_ARG])) {
		fprintf(stderr, "Please specify both major number and "
				"minor number.\n");
		return 0;
	}

	if (_switches[COLS_ARG] && _switches[OPTIONS_ARG] &&
	    strcmp(_fields, "name")) {
		fprintf(stderr, "Only -o name is supported so far.\n");
		return 0;
	}

	if (_switches[TREE_ARG] && !_process_tree_options(_fields))
		return 0;

	if (_switches[TABLE_ARG] && _switches[NOTABLE_ARG]) {
		fprintf(stderr, "--table and --notable are incompatible.\n");
		return 0;
	}

	*argv += optind;
	*argc -= optind;
	return 1;
}

int main(int argc, char **argv)
{
	struct command *c;
	int r = 1;

	(void) setlocale(LC_ALL, "");

	if (!_process_switches(&argc, &argv)) {
		fprintf(stderr, "Couldn't process command line.\n");
		goto out;
	}

	if (_switches[VERSION_ARG]) {
		c = _find_command("version");
		goto doit;
	}

	if (argc == 0) {
		_usage(stderr);
		goto out;
	}

	if (!(c = _find_command(argv[0]))) {
		fprintf(stderr, "Unknown command\n");
		_usage(stderr);
		goto out;
	}

	if (argc < c->min_args + 1 ||
	    (c->max_args >= 0 && argc > c->max_args + 1)) {
		fprintf(stderr, "Incorrect number of arguments\n");
		_usage(stderr);
		goto out;
	}

      doit:
	if (!c->fn(argc, argv, NULL)) {
		fprintf(stderr, "Command failed\n");
		goto out;
	}

	r = 0;

out:
	return r;
}
