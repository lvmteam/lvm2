/*
 * Copyright (C) 2001 Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "tools.h"

void pvcreate_single(const char *pv_name);

int pvcreate(int argc, char **argv)
{
	int opt;

	if (!argc) {
		log_error("Please enter a physical volume path");
		return LVM_EINVALID_CMD_LINE;
	}

	if (arg_count(yes_ARG) && !arg_count(force_ARG)) {
		log_error("option y can only be given with option f");
		return LVM_EINVALID_CMD_LINE;
	}

	for (opt = 0; opt < argc; opt++)
		pvcreate_single(argv[opt]);

	return 0;
}

void pvcreate_single(const char *pv_name)
{
	int size;
	pv_t *pv = NULL;
	pv_t *pv_new;

	struct dev_mgr *dm;
	struct device *pv_dev;

	dm = active_dev_mgr();

	if (!(pv_dev = dev_by_name(dm, pv_name))) {
		log_error("Device %s not found", pv_name);
		return;
	}

	if ((size = device_get_size(pv_name)) < 0) {
		log_error("Unable to get size of %s", pv_name);
		return;
	}

	if (arg_count(force_ARG) < 1 && !partition_type_is_lvm(dm, pv_dev)) {
		return;
	}

	if (!(pv = pv_read(dm, pv_name))) {
		return;
	}

/****
	FIXME: Check attributes

	EXPORTED
	pv->vg_name[strlen(pv->vg_name) - strlen(EXPORTED)] = 0;
	log_error ("physical volume \"%s\" belongs to exported volume group \"%s\"",
	pv_name, pv->vg_name);
*****/

	if (pv->vg_name[0]) {
		if (arg_count(force_ARG) < 2) {
			log_error("Can't initialize physical volume %s of "
				  "volume group %s without -ff", pv_name,
				  pv->vg_name);
			return;
		}
		if (!arg_count(yes_ARG)) {
			if (yes_no_prompt
			    ("Really INITIALIZE physical volume %s"
			     " of volume group %s [y/n]? ", pv_name,
			     pv->vg_name) == 'n') {
				log_print("physical volume %s not initialized",
					  pv_name);
				return;
			}
		}

	}
/***

	if (pv) {
		if (pv_check_active((char *) pv->vg_name, (char *) pv_name) ==
		    TRUE) {
			log_error
			    ("can't force create on active physical volume \"%s\"",
			     pv_name);
			return;
		}

***/

	if (arg_count(force_ARG)) {
		/* FIXME: Change to log_print */
		printf("WARNING: forcing physical volume creation on %s",
		       pv_name);
		if (pv->vg_name[0])
			printf(" of volume group %s", pv->vg_name);
		printf("\n");
	}

	/* FIXME: If PV is in VG, remove it.  NoOp?  Or cache? */

	log_verbose("creating new physical volume");
	log_verbose("setting up physical volume for %s with %u sectors",
		    pv_name, size);

/*** FIXME: New metadata creation fn reqd
	if (!(pv_new = pv_setup_for_create(pv_name, size)) < 0) {
		log_error("Failed to set up  physical volume %s", pv_name);
		return;
	}
***/

	log_verbose("writing physical volume data to disk %s", pv_name);

/*** FIXME: New metadata write fn reqd
	if (!(pv_write(pv_name, pv_new)) == 0) {
		log_error("Failed to create physical volume %s", pv_name);
	}
***/

	printf("physical volume %s successfully created\n", pv_name);

/* FIXME: Add the dbg_frees throughout! */
	return;
}
