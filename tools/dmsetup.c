/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
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
#include <ctype.h>
#include <errno.h>

#define LINE_SIZE 1024

#define err(msg, x...) fprintf(stderr, msg "\n", ##x)

static int _parse_file(struct dm_task *dmt, const char *file)
{
        char buffer[LINE_SIZE], *ttype, *ptr, *comment;
        FILE *fp = fopen(file, "r");
        unsigned long long start, size;
        int r = 0, n, line = 0;

        if (!fp) {
                err("Couldn't open '%s' for reading", file);
                return 0;
        }

        while (fgets(buffer, sizeof(buffer), fp)) {
		line++;

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
                        continue;

                if (sscanf(ptr, "%llu %llu %as %n",
                           &start, &size, &ttype, &n) < 3) {
                        err("%s:%d Invalid format", file, line);
                        goto out;
                }

		ptr += n;
		if ((comment = strchr(ptr, (int) '#')))
			*comment = '\0';

                if (!dm_task_add_target(dmt, start, size, ttype, ptr))
                        goto out;

		free(ttype);
        }
        r = 1;

 out:
        fclose(fp);
        return r;
}

static int _load(int task, const char *name, const char *file)
{
	int r = 0;
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task)))
		return 0;

	if (!dm_task_set_name(dmt, name))
		goto out;

	if (!_parse_file(dmt, file))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	r = 1;

out:
	dm_task_destroy(dmt);

	return r;
}

static int _create(int argc, char **argv)
{
	return _load(DM_DEVICE_CREATE, argv[1], argv[2]);
}

static int _reload(int argc, char **argv)
{
	return _load(DM_DEVICE_RELOAD, argv[1], argv[2]);
}


static int _simple(int task, const char *name)
{
	int r = 0;

	/* remove <dev_name> */
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task)))
		return 0;

	if (!dm_task_set_name(dmt, name))
		goto out;

	r = dm_task_run(dmt);

 out:
	dm_task_destroy(dmt);
	return r;
}

static int _remove(int argc, char **argv)
{
	return _simple(DM_DEVICE_REMOVE, argv[1]);
}

static int _suspend(int argc, char **argv)
{
	return _simple(DM_DEVICE_SUSPEND, argv[1]);
}

static int _resume(int argc, char **argv)
{
	return _simple(DM_DEVICE_RESUME, argv[1]);
}

static int _info(int argc, char **argv)
{
	int r = 0;

	/* remove <dev_name> */
	struct dm_task *dmt;
	struct dm_info info;

	if (!(dmt = dm_task_create(DM_DEVICE_INFO)))
		return 0;

	if (!dm_task_set_name(dmt, argv[1]))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &info))
		goto out;

	if (!info.exists) {
		printf("Device does not exist.\n");
		r = 1;
		goto out;
	}

	printf("state:             %s\n",
	       info.suspended ? "SUSPENDED" : "ACTIVE");
	printf("open count:        %d\n", info.open_count);
	printf("major, minor:      %d, %d\n", info.major, info.minor);
	printf("number of targets: %d\n", info.target_count);
	r = 1;

 out:
	dm_task_destroy(dmt);
	return r;
}


/*
 * dispatch table
 */
typedef int (*command_fn)(int argc, char **argv);

struct command {
	char *name;
	char *help;
	int num_args;
	command_fn fn;
};

static struct command _commands[] = {
	{"create", "<dev_name> <table_file>", 2, _create},
	{"remove", "<dev_name>", 1, _remove},
	{"suspend", "<dev_name>", 1, _suspend},
	{"resume", "<dev_name>", 1, _resume},
	{"reload", "<dev_name> <table_file>", 2, _reload},
	{"info", "<dev_name>", 1, _info},
	{NULL, NULL, 0, NULL}
};

static void _usage(FILE *out)
{
	int i;

	fprintf(out, "usage:\n");
	for (i = 0; _commands[i].name; i++)
		fprintf(out, "\t%s %s\n",
			_commands[i].name, _commands[i].help);
	return;
}

struct command *_find_command(const char *name)
{
	int i;

	for (i = 0; _commands[i].name; i++)
		if (!strcmp(_commands[i].name, name))
			return _commands + i;

	return NULL;
}


int main(int argc, char **argv)
{
	struct command *c;

	if (argc < 2) {
		_usage(stderr);
		exit(1);
	}

	if (!(c = _find_command(argv[1]))) {
		fprintf(stderr, "Unknown command\n");
		_usage(stderr);
		exit(1);
	}

	if (argc != c->num_args + 2) {
		fprintf(stderr, "Incorrect number of arguments\n");
		_usage(stderr);
		exit(1);
	}

	if (!c->fn(argc - 1, argv + 1)) {
		//fprintf(stderr, "Command failed\n");
		exit(1);
	}

	return 0;
}

