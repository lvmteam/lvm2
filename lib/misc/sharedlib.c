/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "lib.h"
#include "config.h"
#include "lvm-string.h"

#include <limits.h>
#include <sys/stat.h>
#include <dlfcn.h>

void *load_shared_library(struct config_tree *cf, const char *libname,
			  const char *desc)
{
	char path[PATH_MAX];
	struct stat info;
	const char *lib_dir;
	void *library;

	/* If libname doesn't begin with '/' then use lib_dir/libname,
	 * if present */
	if (libname[0] == '/' ||
	    !(lib_dir = find_config_str(cf->root, "global/library_dir",
					'/', 0)) ||
	    (lvm_snprintf(path, sizeof(path), "%s/%s", lib_dir,
			  libname) == -1) || stat(path, &info) == -1)
		strncpy(path, libname, sizeof(path));

	log_very_verbose("Opening shared %s library %s", desc, path);

	if (!(library = dlopen(path, RTLD_LAZY)))
		log_error("Unable to open external %s library %s", desc, path);

	return library;
}
