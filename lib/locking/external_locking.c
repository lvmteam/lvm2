/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "lib.h"
#include "locking_types.h"
#include "defaults.h"
#include "sharedlib.h"

static void *_locking_lib = NULL;
static void (*_end_fn) (void) = NULL;
static int (*_lock_fn) (struct cmd_context * cmd, const char *resource,
			int flags) = NULL;
static int (*_init_fn) (int type, struct config_tree * cft) = NULL;

static int _lock_resource(struct cmd_context *cmd, const char *resource,
			  int flags)
{
	if (_lock_fn)
		return _lock_fn(cmd, resource, flags);
	else
		return 0;
}

static void _fin_external_locking(void)
{
	if (_end_fn)
		_end_fn();

	dlclose(_locking_lib);

	_locking_lib = NULL;
	_init_fn = NULL;
	_end_fn = NULL;
	_lock_fn = NULL;
}

int init_external_locking(struct locking_type *locking, struct config_tree *cft)
{
	const char *libname;

	if (_locking_lib) {
		log_error("External locking already initialised");
		return 1;
	}

	locking->lock_resource = _lock_resource;
	locking->fin_locking = _fin_external_locking;

	libname = find_config_str(cft->root, "global/locking_library",
				  DEFAULT_LOCKING_LIB);

	if (!(_locking_lib = load_shared_library(cft, libname, "locking"))) {
		stack;
		return 0;
	}

	/* Get the functions we need */
	if (!(_init_fn = dlsym(_locking_lib, "init_locking")) ||
	    !(_lock_fn = dlsym(_locking_lib, "lock_resource")) ||
	    !(_end_fn = dlsym(_locking_lib, "end_locking"))) {
		log_error("Shared library %s does not contain locking "
			  "functions", libname);
		dlclose(_locking_lib);
		_locking_lib = NULL;
		return 0;
	}

	log_verbose("Loaded external locking library %s", libname);
	return _init_fn(2, cft);
}
