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
	struct physical_volume *pv = NULL;

	struct io_space *ios;
	struct device *pv_dev;

	ios = active_ios();

	if (!(pv_dev = dev_cache_get(pv_name))) {
		log_error("Device %s not found", pv_name);
		return;
	}

	if ((size = dev_get_size(pv_dev)) < 0) {
		log_error("Unable to get size of %s", pv_name);
		return;
	}

	if (arg_count(force_ARG) < 1 && !partition_type_is_lvm(ios, pv_dev)) {
		return;
	}

	pv = ios->pv_read(ios, pv_dev);

	if (pv && (pv->status & STATUS_EXPORTED)) {
		log_error ("Physical volume %s belongs to exported volume"
			   " group %s", pv_name, pv->vg_name);
		return;
	}

	if (pv && pv->vg_name[0]) {
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

	if (pv && (pv->status & STATUS_ACTIVE)) {
		log_error("Can't create on active physical volume %s",
			  pv_name);
		return;
	}


	if (!pv) {
		if (!(pv = pv_create()))
			return;
		/* FIXME: Set up initial size & PEs here */
	}
	

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


	log_verbose("writing physical volume data to disk %s", pv_name);

	if (!(pv_write(ios, pv))) {
		log_error("Failed to create physical volume %s", pv_name);
		return;
	}

	printf("physical volume %s successfully created\n", pv_name);

/* FIXME: Add the dbg_frees throughout! */
	return;
}
