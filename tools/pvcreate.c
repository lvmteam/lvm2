/*
 * Copyright (C) 2001 Sistina Software
 *
 * pvcreate is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * pvcreate is distributed in the hope that it will be useful,
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

const char _really_init[] =
    "Really INITIALIZE physical volume %s of volume group %s [y/n]? ";

/*
 * See if we may pvcreate on this device.
 * 0 indicates we may not.
 */
static int pvcreate_check(const char *name)
{
	struct physical_volume *pv;

	/* is the partition type set correctly ? */
	if ((arg_count(force_ARG) < 1) && !is_lvm_partition(name))
		return 0;

	/* is there a pv here already */
	if (!(pv = fid->ops->pv_read(fid, name)))
		return 1;

	/* orphan ? */
	if (!pv->vg_name[0])
		return 1;

	/* never overwrite exported pv's */
	if (pv->status & EXPORTED_VG) {
		log_error("Physical volume %s belongs to exported volume"
			  " group %s", name, pv->vg_name);
		return 0;
	}

	/* we must have -ff to overwrite a non orphan */
	if (arg_count(force_ARG) < 2) {
		log_error("Can't initialize physical volume %s of "
			  "volume group %s without -ff", name, pv->vg_name);
		return 0;
	}

	/* prompt */
	if (!arg_count(yes_ARG) &&
	    yes_no_prompt(_really_init, name, pv->vg_name) == 'n') {
		log_print("Physical volume %s not initialized", name);
		return 0;
	}

	if (arg_count(force_ARG)) {
		log_print("WARNING: Forcing physical volume creation on "
			  "%s%s%s", name,
			  pv->vg_name[0] ? " of volume group " : "",
			  pv->vg_name[0] ? pv->vg_name : "");
	}

	return 1;
}

static void pvcreate_single(const char *pv_name)
{
	struct physical_volume *pv;

	if (!pvcreate_check(pv_name))
		return;

	if (!(pv = pv_create(fid, pv_name))) {
		log_err("Failed to setup physical volume %s", pv_name);
		return;
	}

	log_verbose("Set up physical volume for %s with %" PRIu64 " sectors",
		    pv_name, pv->size);

	log_verbose("Writing physical volume data to disk %s", pv_name);
	if (!(fid->ops->pv_write(fid, pv))) {
		log_error("Failed to write physical volume %s", pv_name);
		return;
	}

	log_print("Physical volume %s successfully created", pv_name);
}

int pvcreate(int argc, char **argv)
{
	int i;

	if (!argc) {
		log_error("Please enter a physical volume path");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(yes_ARG) && !arg_count(force_ARG)) {
		log_error("Option y can only be given with option f");
		return EINVALID_CMD_LINE;
	}

	for (i = 0; i < argc; i++) {
		pvcreate_single(argv[i]);
		pool_empty(fid->cmd->mem);
	}

	return 0;
}
