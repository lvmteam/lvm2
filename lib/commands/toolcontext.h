/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#ifndef _LVM_TOOLCONTEXT_H
#define _LVM_TOOLCONTEXT_H

#include "dev-cache.h"
#include "config.h"
#include "pool.h"
#include "metadata.h"

#include <stdio.h>
#include <limits.h>

/*
 * Config options that can be changed while commands are processed
 */
struct config_info {
	int debug;
	int verbose;
	int test;
	int syslog;
	int activation;
	int suffix;
	uint64_t unit_factor;
	char unit_type;
	const char *msg_prefix;
	int cmd_name;		/* Show command name? */

	int archive;		/* should we archive ? */
	int backup;		/* should we backup ? */

	struct format_type *fmt;

	mode_t umask;
};

/* FIXME Split into tool & library contexts */
/* command-instance-related variables needed by library */
struct cmd_context {
	/* format handler allocates all objects from here */
	struct pool *mem;

	struct format_type *fmt;	/* Current format to use by default */
	struct format_type *fmt_backup;	/* Format to use for backups */

	struct list formats;	/* Available formats */

	char *cmd_line;
	struct command *command;
	struct arg *args;

	struct dev_filter *filter;
	int dump_filter;	/* Dump filter when exiting? */

	struct config_tree *cf;
	struct config_info default_settings;
	struct config_info current_settings;

	char sys_dir[PATH_MAX];
	char dev_dir[PATH_MAX];
	char proc_dir[PATH_MAX];
};

struct cmd_context *create_toolcontext(struct arg *the_args);
void destroy_toolcontext(struct cmd_context *cmd);

#endif
