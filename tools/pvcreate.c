/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

const char _really_init[] =
    "Really INITIALIZE physical volume %s of volume group %s [y/n]? ";

/*
 * See if we may pvcreate on this device.
 * 0 indicates we may not.
 */
static int _check(const char *name)
{
	struct physical_volume *pv;

	/* is the partition type set correctly ? */
	if ((arg_count(force_ARG) < 1) && !is_lvm_partition(name))
		return 0;

	/* is there a pv here already */
	if (!(pv = ios->pv_read(ios, name)))
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
		log_print("physical volume %s not initialized", name);
		return 0;
	}

	if (pv->status & ACTIVE) {
		log_error("Can't create on active physical volume %s", name);
		return 0;
	}

	return 1;
}

static void pvcreate_single(const char *name)
{
	struct physical_volume *pv;

	if (!_check(name))
		return;

	if (arg_count(force_ARG)) {
		log_print("WARNING: forcing physical volume creation on %s",
			  name);

		if (pv->vg_name[0])
			log_print(" of volume group %s", pv->vg_name);
		printf("\n");
	}

	if (!(pv = pv_create(name, ios))) {
		log_err("Failed to setup physical volume %s", name);
		return;
	}
	log_verbose("set up physical volume for %s with %llu sectors",
		    name, pv->size);


	log_verbose("writing physical volume data to disk %s", name);
	if (!(ios->pv_write(ios, pv))) {
		log_error("Failed to write physical volume %s", name);
		return;
	}

	log_print("physical volume %s successfully created", name);
}

int pvcreate(int argc, char **argv)
{
	int i;

	if (!argc) {
		log_error("Please enter a physical volume path");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(yes_ARG) && !arg_count(force_ARG)) {
		log_error("option y can only be given with option f");
		return EINVALID_CMD_LINE;
	}

	for (i = 0; i < argc; i++) {
		pvcreate_single(argv[i]);
		pool_empty(ios->mem);
	}

	return 0;
}
