/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

#define DEFAULT_PV 128
#define DEFAULT_LV 128
#define DEFAULT_EXTENT 8192

int vgcreate(int argc, char **argv)
{
	int max_lv, max_pv, extent_size;
	struct volume_group *vg;

	if (!argc) {
		log_error("Please provide volume group name and "
			  "physical volumes");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1) {
		log_error("Please enter physical volume name(s)");
		return EINVALID_CMD_LINE;
	}

	max_lv = arg_int_value(maxlogicalvolumes_ARG, DEFAULT_LV);
	max_pv = arg_int_value(maxphysicalvolumes_ARG, DEFAULT_PV);
	extent_size = arg_int_value(physicalextentsize_ARG, DEFAULT_EXTENT);

	/* create the new vg */
	if (!vg_create(ios, argv[0], extent_size, max_pv, max_lv, 
		       argc - 1, argv + 1))
		return ECMD_FAILED;

	/* store vg on disk(s) */
	if (ios->vg_write(ios, vg))
		return ECMD_FAILED;

	log_print("Volume group %s successfully created and activated",
		  argv[0]);

	return 0;
}
