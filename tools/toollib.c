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

#include "tools.h"

#include <ctype.h>

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

int do_autobackup(struct volume_group *vg)
{

/***************
	log_verbose("Changing lvmtab");
	if ((vg_cfgbackup(vg_name, LVMTAB_DIR, vg))) {
		log_error("\"%s\" writing \"%s\"", lvm_error(ret), LVMTAB);
		return LVM_E_VG_CFGBACKUP;
	}
**************/

	if (!autobackup_set()) {
		log_print
		    ("WARNING: You don't have an automatic backup of %s",
		     vg->name);
		return 0;
	}

/***************
	log_print("Creating automatic backup of volume group \"%s\"", vg_name);
	if ((vg_cfgbackup(vg_name, VG_BACKUP_DIR, vg))) {
		log_error("\"%s\" writing VG backup of \"%s\"", lvm_error(ret),
			  vg_name);
		return LVM_E_VG_CFGBACKUP;
	}
***************/

	return 0;
}

int process_each_vg(int argc, char **argv,
		    int (*process_single) (const char *vg_name))
{
	int opt = 0;
	int ret_max = 0;
	int ret = 0;

	struct list_head *vgh;
	struct list_head *vgs_list;

	if (argc) {
		log_verbose("Using volume group(s) on command line");
		for (; opt < argc; opt++)
			if ((ret = process_single(argv[opt])) > ret_max)
				ret_max = ret;
	} else {
		log_verbose("Finding all volume group(s)");
		if (!(vgs_list = ios->get_vgs(ios))) {
			log_error("No volume groups found");
			return ECMD_FAILED;
		}
		list_for_each(vgh, vgs_list) {
			ret =
			    process_single(list_entry
					   (vgh, struct name_list, list)->name);
			if (ret > ret_max)
				ret_max = ret;
		}
	}

	return ret_max;
}

int process_each_pv(int argc, char **argv, struct volume_group *vg,
		    int (*process_single) (struct volume_group * vg,
					   struct physical_volume * pv))
{
	int opt = 0;
	int ret_max = 0;
	int ret = 0;

	struct list_head *pvh;

	if (argc) {
		log_verbose("Using physical volume(s) on command line");
		for (; opt < argc; opt++) {
			if (!(pvh = find_pv_in_vg(vg, argv[opt]))) {
				log_error("Physical Volume %s not found in "
					  "Volume Group %s", argv[opt],
					  vg->name);
				continue;
			}
			ret = process_single(vg, &list_entry
					     (pvh, struct pv_list, list)->pv);
			if (ret > ret_max)
				ret_max = ret;
		}
	} else {
		log_verbose("Using all physical volume(s) in volume group");
		list_for_each(pvh, &vg->pvs) {
			ret = process_single(vg, &list_entry
					     (pvh, struct pv_list, list)->pv);
			if (ret > ret_max)
				ret_max = ret;
		}
	}

	return ret_max;
}

int is_valid_chars(char *n)
{
	register char c;
	while ((c = *n++))
		if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+')
			return 0;
	return 1;
}

