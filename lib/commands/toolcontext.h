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

/* command-instance-related variables needed by library */
struct cmd_context {
	/* format handler allocates all objects from here */
	struct pool *mem;

	struct format_instance *fid;

	char *cmd_line;
	char *dev_dir;
	struct dev_filter *filter;
	struct config_file *cf;

	struct command *command;
	struct uuid_map *um;
	struct arg *args;
};

#endif
