/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "tools.h"

/* FIXME From config file? */
#define DEFAULT_PV 255
#define DEFAULT_LV 255

#define DEFAULT_EXTENT 4096  /* In KB */

int vgcreate(int argc, char **argv)
{
	int max_lv, max_pv, extent_size;
	char *vg_name;
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

	vg_name = argv[0];
	max_lv = arg_int_value(maxlogicalvolumes_ARG, DEFAULT_LV);
	max_pv = arg_int_value(maxphysicalvolumes_ARG, DEFAULT_PV);

	/* Units of 512-byte sectors */
	extent_size = arg_int_value(physicalextentsize_ARG, DEFAULT_EXTENT) * 2;

	if (max_lv < 1) {
		log_error("maxlogicalvolumes too low");
		return EINVALID_CMD_LINE;
	}
		
	if (max_pv < 1) {
		log_error("maxphysicalvolumes too low");
		return EINVALID_CMD_LINE;
	}
		
        /* Strip prefix if present */
        if (!strncmp(vg_name, ios->prefix, strlen(ios->prefix)))
                vg_name += strlen(ios->prefix);

        if (!is_valid_chars(vg_name)) {
                log_error("New volume group name '%s' has invalid characters",
                          vg_name);
                return ECMD_FAILED;
        }

	/* create the new vg */
	if (!(vg = vg_create(ios, vg_name, extent_size, max_pv, max_lv, 
		       argc - 1, argv + 1)))
		return ECMD_FAILED;

	if (max_lv != vg->max_lv) 
		log_error("Warning: Setting maxlogicalvolumes to %d", 
			  vg->max_lv);

	if (max_pv != vg->max_pv) 
		log_error("Warning: Setting maxphysicalvolumes to %d", 
			  vg->max_pv);

	/* store vg on disk(s) */
	if (!ios->vg_write(ios, vg))
		return ECMD_FAILED;

	/* FIXME Create /dev/vg */
	/* FIXME Activate */

	log_print("Volume group %s successfully created and activated",
		  vg_name);

	return 0;
}
