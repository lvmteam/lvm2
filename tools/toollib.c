/*
 * Copyright (C) 2001  Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 *
 */

#include "lvm_user.h"

static int _autobackup = 1;

int autobackup_set()
{
	return _autobackup;
}

int init_autobackup()
{
	char *lvm_autobackup;

	if (arg_count(autobackup_ARG)) {
		_autobackup = strcmp(arg_str_value(autobackup_ARG, "y"), "n");
		return 0;
	}

	_autobackup = 1;	/* default */

	lvm_autobackup = getenv("LVM_AUTOBACKUP");
	if (!lvm_autobackup)
		return 0;

	log_print("using environment variable LVM_AUTOBACKUP "
		  "to set option A");
	if (!strcasecmp(lvm_autobackup, "no"))
		_autobackup = 0;
	else if (strcasecmp(lvm_autobackup, "yes")) {
		log_error("environment variable LVM_AUTOBACKUP has "
			  "invalid value \"%s\"!", lvm_autobackup);
		return -1;
	}

	return 0;
}

int do_autobackup(char *vg_name, vg_t * vg)
{
	int ret;

	log_verbose("Changing lvmtab");
	if ((ret = vg_cfgbackup(vg_name, LVMTAB_DIR, vg))) {
		log_error("\"%s\" writing \"%s\"", lvm_error(ret), LVMTAB);
		return LVM_E_VG_CFGBACKUP;
	}

	if (!autobackup_set()) {
		log_print
		    ("WARNING: You don't have an automatic backup of \"%s\"",
		     vg_name);
		return 0;
	}

	log_print("Creating automatic backup of volume group \"%s\"", vg_name);
	if ((ret = vg_cfgbackup(vg_name, VG_BACKUP_DIR, vg))) {
		log_error("\"%s\" writing VG backup of \"%s\"", lvm_error(ret),
			  vg_name);
		return LVM_E_VG_CFGBACKUP;
	}

	return 0;
}
