/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "log.h"
#include "locking.h"
#include "locking_types.h"
#include "activate.h"
#include "config.h"
#include "defaults.h"
#include "lvm-file.h"
#include "lvm-string.h"
#include "dbg_malloc.h"

#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>

static void *locking_module = NULL;
static void (*end_fn) (void) = NULL;
static int (*lock_fn) (struct cmd_context * cmd, const char *resource,
		       int flags) = NULL;
static int (*init_fn) (int type, struct config_file * cf) = NULL;

static int lock_resource(struct cmd_context *cmd, const char *resource,
			 int flags)
{
	if (lock_fn)
		return lock_fn(cmd, resource, flags);
	else
		return 0;
}

static void fin_external_locking(void)
{
	if (end_fn)
		end_fn();

	dlclose(locking_module);

	locking_module = NULL;
	end_fn = NULL;
	lock_fn = NULL;
}

int init_external_locking(struct locking_type *locking, struct config_file *cf)
{
	char _lock_lib[PATH_MAX];

	if (locking_module) {
		log_error("External locking already initialised");
		return 1;
	}
	locking->lock_resource = lock_resource;
	locking->fin_locking = fin_external_locking;

	/* Get locking module name from config file */
	strncpy(_lock_lib, find_config_str(cf->root, "global/locking_library",
					   '/', "lvm2_locking.so"),
		sizeof(_lock_lib));

	/* If there is a module_dir in the config file then
	   look for the locking module in there first and then
	   using the normal dlopen(3) mechanism of looking
	   down LD_LIBRARY_PATH and /lib, /usr/lib.
	   If course, if the library name starts with a slash then
	   just use the name... */
	if (_lock_lib[0] != '/') {
		struct stat st;
		char _lock_lib1[PATH_MAX];

		lvm_snprintf(_lock_lib1, sizeof(_lock_lib1),
			     "%s/%s",
			     find_config_str(cf->root, "global/module_dir",
					     '/', "RUBBISH"), _lock_lib);

		/* Does it exist ? */
		if (stat(_lock_lib1, &st) == 0) {
			strcpy(_lock_lib, _lock_lib1);
		}
	}

	log_very_verbose("Opening locking library %s", _lock_lib);

	locking_module = dlopen(_lock_lib, RTLD_LAZY);
	if (!locking_module) {
		log_error("Unable to open external locking module %s",
			  _lock_lib);
		return 0;
	}

	/* Get the functions we need */
	init_fn = dlsym(locking_module, "init_locking");
	lock_fn = dlsym(locking_module, "lock_resource");
	end_fn = dlsym(locking_module, "end_locking");

	/* Are they all there ? */
	if (!end_fn || !init_fn || !lock_fn) {
		log_error ("Shared library %s does not contain locking "
			   "functions", _lock_lib);
		dlclose(locking_module);
		return 0;
	}

	log_verbose("Opened external locking module %s", _lock_lib);
	return init_fn(2, cf);
}
