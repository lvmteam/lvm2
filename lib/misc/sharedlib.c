/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "config.h"
#include "lvm-string.h"
#include "sharedlib.h"

#include <limits.h>
#include <sys/stat.h>
#include <dlfcn.h>

void *load_shared_library(struct config_tree *cft, const char *libname,
			  const char *desc)
{
	char path[PATH_MAX];
	struct stat info;
	const char *lib_dir;
	void *library;

	/* If libname doesn't begin with '/' then use lib_dir/libname,
	 * if present */
	if (libname[0] == '/' ||
	    !(lib_dir = find_config_str(cft->root, "global/library_dir", 0)) ||
	    (lvm_snprintf(path, sizeof(path), "%s/%s", lib_dir,
			  libname) == -1) || stat(path, &info) == -1)
		strncpy(path, libname, sizeof(path));

	log_very_verbose("Opening shared %s library %s", desc, path);

	if (!(library = dlopen(path, RTLD_LAZY)))
		log_error("Unable to open external %s library %s", desc, path);

	return library;
}
