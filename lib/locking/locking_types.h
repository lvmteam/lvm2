/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "metadata.h"
#include "config.h"

typedef int (*lock_resource_fn)(struct cmd_context *cmd, const char *resource, 
				int flags);

typedef void (*fin_lock_fn)(void);


struct locking_type {
	lock_resource_fn lock_resource;

	fin_lock_fn fin_locking;
};


/*
 * Locking types
 */
int init_file_locking(struct locking_type *locking, struct config_file *cf);

int init_external_locking(struct locking_type *locking, struct config_file *cf);

