/*
 * Copyright (C) 2001  Sistina Software
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

int vgextend(int argc, char **argv)
{
	char *vg_name;
	struct volume_group *vg = NULL;

	if (!argc) {
		log_error("Please enter volume group name and "
		          "physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1) {
		log_error("Please enter physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	vg_name = argv[0];
	argc--;
	argv++;

	log_verbose("Checking for volume group '%s'", vg_name);
	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Volume group '%s' not found.", vg_name);
		return ECMD_FAILED;
	}

/******* Ignore active
	if (!(vg->status & ACTIVE))
		log_error("Volume group '%s' is not active.", vg_name);
********/

	if (!(vg->status & EXTENDABLE_VG)) {
		log_error("Volume group '%s' is not extendable.", vg_name);
		return ECMD_FAILED;
	}

/********** FIXME
	log_print("maximum logical volume size is %s",
		  (dummy = lvm_show_size(LVM_LV_SIZE_MAX(vg) / 2, LONG)));
	dbg_free(dummy);
	dummy = NULL;
**********/

	/* extend vg */
	if (!vg_extend(fid, vg, argc, argv))
		return ECMD_FAILED;

	/* ret > 0 */
	log_verbose("Volume group '%s' will be extended by %d new "
		    "physical volumes", vg_name, argc);

        /* store vg on disk(s) */
        if (!fid->ops->vg_write(fid, vg))
                return ECMD_FAILED;

/********* FIXME
	if ((ret = do_autobackup(vg_name, vg)))
		return ret;
*********/

	log_print("Volume group '%s' successfully extended", vg_name);

	return 0;
}
