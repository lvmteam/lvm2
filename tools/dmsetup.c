/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "libdevmapper.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

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

#ifdef linux
#  include "kdev_t.h"
#else
#  define MAJOR(x) major((x))
#  define MINOR(x) minor((x))
#  define MKDEV(x,y) makedev((x),(y))
#endif

#define LINE_SIZE 1024

#define err(msg, x...) fprintf(stderr, msg "\n", ##x)

/*
 * We have only very simple switches ATM.
 */
enum {
	READ_ONLY = 0,
	COLS_ARG,
	MAJOR_ARG,
	MINOR_ARG,
	NOHEADINGS_ARG,
	NOOPENCOUNT_ARG,
	NOTABLE_ARG,
	UUID_ARG,
	VERBOSE_ARG,
	VERSION_ARG,
	NUM_SWITCHES
};

static int _switches[NUM_SWITCHES];
static int _values[NUM_SWITCHES];
static char *_uuid;

/*
 * Commands
 */
static int _parse_file(struct dm_task *dmt, const char *file)
{
	char buffer[LINE_SIZE], ttype[LINE_SIZE], *ptr, *comment;
	FILE *fp;
	unsigned long long start, size;
	int r = 0, n, line = 0;

	/* OK for empty stdin */

	if (file) {
		if (!(fp = fopen(file, "r"))) {
			err("Couldn't open '%s' for reading", file);
			return 0;
		}
	} else
		fp = stdin;

	while (fgets(buffer, sizeof(buffer), fp)) {
		line++;

		/* trim trailing space */
		for (ptr = buffer + strlen(buffer) - 1; ptr >= buffer; ptr--)
			if (!isspace((int) *ptr))
				break;
		ptr++;
		*ptr = '\0';

		/* trim leading space */
		for (ptr = buffer; *ptr && isspace((int) *ptr); ptr++) ;

		if (!*ptr || *ptr == '#')
			continue;

		if (sscanf(ptr, "%llu %llu %s %n",
			   &start, &size, ttype, &n) < 3) {
			err("%s:%d Invalid format", file, line);
			goto out;
		}

		ptr += n;
		if ((comment = strchr(ptr, (int) '#')))
			*comment = '\0';

		if (!dm_task_add_target(dmt, start, size, ttype, ptr))
			goto out;
	}
	r = 1;

      out:
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
		printf("Name             Maj Min Stat Open Targ Event  UUID\n");
		_headings = 1;
	}

	printf("%-16s ", dm_task_get_name(dmt));

	printf("%3d %3d %s%s%s%s %4d %4d %6" PRIu32 " ",
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

static void _display_info(struct dm_task *dmt)
{
	struct dm_info info;

	if (!dm_task_get_info(dmt, &info))
		return;

	if (!_switches[COLS_ARG])
		_display_info_long(dmt, &info);
	else if (_switches[NOHEADINGS_ARG])
		_display_info_cols_noheadings(dmt, &info);
	else
		_display_info_cols(dmt, &info);
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

static int _load(int argc, char **argv, void *data)
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
		_display_info(dmt);

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _create(int argc, char **argv, void *data)
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

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	r = 1;

	if (_switches[VERBOSE_ARG])
		_display_info(dmt);

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _rename(int argc, char **argv, void *data)
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

static int _message(int argc, char **argv, void *data)
{
	int r = 0, sz = 1, i;
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

	if (!dm_task_set_sector(dmt, atoll(argv[1])))
		goto out;

	argc -= 2;
	argv += 2;

	if (argc <= 0)
		err("No message supplied.\n");

	for (i = 0; i < argc; i++)
		sz += strlen(argv[i]) + 1;

	str = malloc(sz);
	memset(str, 0, sz);

	for (i = 0; i < argc; i++) {
		if (i)
			strcat(str, " ");
		strcat(str, argv[i]);
	}

	if (!dm_task_set_message(dmt, str))
		goto out;

	free(str);

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	r = 1;

      out:
	dm_task_destroy(dmt);

	return r;
}

static int _version(int argc, char **argv, void *data)
{
	int r = 0;
	struct dm_task *dmt;
	char version[80];

	if (dm_get_library_version(version, sizeof(version)))
		printf("Library version:   %s\n", version);

	if (!(dmt = dm_task_create(DM_DEVICE_VERSION)))
		return 0;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_driver_version(dmt, (char *) &version,
					sizeof(version))) {
		goto out;
	}

	printf("Driver version:    %s\n", version);

	r = 1;

      out:
	dm_task_destroy(dmt);

	return r;
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

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	r = dm_task_run(dmt);

	if (r && display && _switches[VERBOSE_ARG])
		_display_info(dmt);

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _remove_all(int argc, char **argv, void *data)
{
	return _simple(DM_DEVICE_REMOVE_ALL, "", 0, 0);
}

static int _remove(int argc, char **argv, void *data)
{
	return _simple(DM_DEVICE_REMOVE, argc > 1 ? argv[1] : NULL, 0, 0);
}

static int _suspend(int argc, char **argv, void *data)
{
	return _simple(DM_DEVICE_SUSPEND, argc > 1 ? argv[1] : NULL, 0, 1);
}

static int _resume(int argc, char **argv, void *data)
{
	return _simple(DM_DEVICE_RESUME, argc > 1 ? argv[1] : NULL, 0, 1);
}

static int _clear(int argc, char **argv, void *data)
{
	return _simple(DM_DEVICE_CLEAR, argc > 1 ? argv[1] : NULL, 0, 1);
}

static int _wait(int argc, char **argv, void *data)
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
		       (argc > 1) ? atoi(argv[argc - 1]) : 0, 1);
}

static int _process_all(int argc, char **argv,
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

static int _status(int argc, char **argv, void *data)
{
	int r = 0;
	struct dm_task *dmt;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	int cmd;
	struct dm_names *names = (struct dm_names *) data;
	char *name = NULL;

	if (data)
		name = names->name;
	else {
		if (argc == 1 && !_switches[UUID_ARG] && !_switches[MAJOR_ARG])
			return _process_all(argc, argv, _status);
		if (argc == 2)
			name = argv[1];
	}

	if (!strcmp(argv[0], "table"))
		cmd = DM_DEVICE_TABLE;
	else
		cmd = DM_DEVICE_STATUS;

	if (!(dmt = dm_task_create(cmd)))
		return 0;

	if (!_set_task_device(dmt, name, 0))
		goto out;

	if (_switches[NOOPENCOUNT_ARG] && !dm_task_no_open_count(dmt))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (_switches[VERBOSE_ARG])
		_display_info(dmt);

	/* Fetch targets and print 'em */
	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);
		if (data && !_switches[VERBOSE_ARG])
			printf("%s: ", name);
		if (target_type) {
			printf("%" PRIu64 " %" PRIu64 " %s %s",
			       start, length, target_type, params);
		}
		printf("\n");
	} while (next);

	if (data && _switches[VERBOSE_ARG])
		printf("\n");

	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;

}

/* Show target names and their version numbers */
static int _targets(int argc, char **argv, void *data)
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

static int _mknodes(int argc, char **argv, void *data)
{
	struct dm_task *dmt;
	int r = 0;

	if (!(dmt = dm_task_create(DM_DEVICE_MKNODES)))
		return 0;

	if (!_set_task_device(dmt, argc > 1 ? argv[1] : NULL, 1))
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
			return _process_all(argc, argv, _info);
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

	_display_info(dmt);

	r = 1;

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
			return _process_all(argc, argv, _deps);
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

static int _display_name(int argc, char **argv, void *data)
{
	struct dm_names *names = (struct dm_names *) data;

	printf("%s\t(%d, %d)\n", names->name,
	       (int) MAJOR(names->dev), (int) MINOR(names->dev));

	return 1;
}

static int _ls(int argc, char **argv, void *data)
{
	return _process_all(argc, argv, _display_name);
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
	  "\t                  [-u|uuid <uuid>] [--notable] [<table_file>]",
	 1, 2, _create},
	{"remove", "<device>", 0, 1, _remove},
	{"remove_all", "", 0, 0, _remove_all},
	{"suspend", "<device>", 0, 1, _suspend},
	{"resume", "<device>", 0, 1, _resume},
	{"load", "<device> [<table_file>]", 0, 2, _load},
	{"clear", "<device>", 0, 1, _clear},
	{"reload", "<device> [<table_file>]", 0, 2, _load},
	{"rename", "<device> <new_name>", 1, 2, _rename},
	{"message", "<device> <sector> <message>", 2, -1, _message},
	{"ls", "", 0, 0, _ls},
	{"info", "[<device>]", 0, 1, _info},
	{"deps", "[<device>]", 0, 1, _deps},
	{"status", "[<device>]", 0, 1, _status},
	{"table", "[<device>]", 0, 1, _status},
	{"wait", "<device> [<event_nr>]", 0, 2, _wait},
	{"mknodes", "[<device>]", 0, 1, _mknodes},
	{"targets", "", 0, 0, _targets},
	{"version", "", 0, 0, _version},
	{NULL, NULL, 0, 0, NULL}
};

static void _usage(FILE *out)
{
	int i;

	fprintf(out, "Usage:\n\n");
	fprintf(out, "dmsetup [--version] [-v|--verbose [-v|--verbose ...]]\n"
		"        [-r|--readonly] [--noopencount]\n\n");
	for (i = 0; _commands[i].name; i++)
		fprintf(out, "\t%s %s\n", _commands[i].name, _commands[i].help);
	fprintf(out, "\n<device> may be device name or -u <uuid> or "
		     "-j <major> -m <minor>\n");
	fprintf(out, "Table_file contents may be supplied on stdin.\n\n");
	return;
}

static struct command *_find_command(const char *name)
{
	int i;

	for (i = 0; _commands[i].name; i++)
		if (!strcmp(_commands[i].name, name))
			return _commands + i;

	return NULL;
}

static int _process_switches(int *argc, char ***argv)
{
	int ind;
	int c;

#ifdef HAVE_GETOPTLONG
	static struct option long_options[] = {
		{"readonly", 0, NULL, READ_ONLY},
		{"columns", 0, NULL, COLS_ARG},
		{"major", 1, NULL, MAJOR_ARG},
		{"minor", 1, NULL, MINOR_ARG},
		{"noheadings", 0, NULL, NOHEADINGS_ARG},
		{"noopencount", 0, NULL, NOOPENCOUNT_ARG},
		{"notable", 0, NULL, NOTABLE_ARG},
		{"uuid", 1, NULL, UUID_ARG},
		{"verbose", 1, NULL, VERBOSE_ARG},
		{"version", 0, NULL, VERSION_ARG},
		{"", 0, NULL, 0}
	};
#else
	struct option long_options;
#endif

	/*
	 * Zero all the index counts.
	 */
	memset(&_switches, 0, sizeof(_switches));
	memset(&_values, 0, sizeof(_values));

	optarg = 0;
	optind = OPTIND_INIT;
	while ((c = GETOPTLONG_FN(*argc, *argv, "cCj:m:nru:v",
				  long_options, &ind)) != -1) {
		if (c == 'c' || c == 'C' || ind == COLS_ARG)
			_switches[COLS_ARG]++;
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
		if (c == 'v' || ind == VERBOSE_ARG)
			_switches[VERBOSE_ARG]++;
		if (c == 'u' || ind == UUID_ARG) {
			_switches[UUID_ARG]++;
			_uuid = optarg;
		}
		if ((ind == NOHEADINGS_ARG))
			_switches[NOHEADINGS_ARG]++;
		if ((ind == NOOPENCOUNT_ARG))
			_switches[NOOPENCOUNT_ARG]++;
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

	*argv += optind;
	*argc -= optind;
	return 1;
}

int main(int argc, char **argv)
{
	struct command *c;

	if (!_process_switches(&argc, &argv)) {
		fprintf(stderr, "Couldn't process command line switches.\n");
		exit(1);
	}

	if (_switches[VERSION_ARG]) {
		c = _find_command("version");
		goto doit;
	}

	if (argc == 0) {
		_usage(stderr);
		exit(1);
	}

	if (!(c = _find_command(argv[0]))) {
		fprintf(stderr, "Unknown command\n");
		_usage(stderr);
		exit(1);
	}

	if (argc < c->min_args + 1 ||
	    (c->max_args >= 0 && argc > c->max_args + 1)) {
		fprintf(stderr, "Incorrect number of arguments\n");
		_usage(stderr);
		exit(1);
	}

      doit:
	if (!c->fn(argc, argv, NULL)) {
		fprintf(stderr, "Command failed\n");
		exit(1);
	}

	dm_lib_release();
	dm_lib_exit();

	return 0;
}
