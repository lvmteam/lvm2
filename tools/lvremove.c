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

int lvremove_single(char *lv_name);

int lvremove(int argc, char **argv)
{
	int opt;
	int ret = 0;

	if (!argc) {
		log_error("Please enter one or more logical volume paths");
		return EINVALID_CMD_LINE;
	}

	for (opt = 0; opt < argc; opt++) {
		if ((ret = lvremove_single(argv[opt])))
			break;
	}

	return ret;
}

int lvremove_single(char *lv_name)
{
	char *vg_name = NULL;

	struct volume_group *vg;
	struct list *lvh;
	struct logical_volume *lv;

	/* does VG exist? */
	if (!(vg_name = extract_vgname(ios, lv_name))) {
		return ECMD_FAILED;
	}

	log_verbose("Finding volume group %s", vg_name);
	if (!(vg = ios->vg_read(ios, vg_name))) {
		log_error("Volume group %s doesn't exist", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg->status & ACTIVE)) {
		log_error("Volume group %s must be active before removing a "
			  "logical volume", vg_name);
		return ECMD_FAILED;
	}

	if (!(lvh = find_lv_in_vg(vg, lv_name))) {
		log_error("Can't find logical volume %s in volume group %s",
			  lv_name, vg_name);
		return ECMD_FAILED;
	}

	lv = &list_item(lvh, struct lv_list)->lv;

	if (lv->status & SNAPSHOT_ORG) {
		log_error("Can't remove logical volume %s under snapshot",
			  lv_name);
		return ECMD_FAILED;
	}

/********** FIXME  Ensure logical volume is not open on *any* machine
	if (lv->open) {
		log_error("can't remove open %s logical volume %s",
			  lv->status & SNAPSHOT ? "snapshot" : "",
			  lv_name);
		return ECMD_FAILED;
	}
************/

	if (!arg_count(force_ARG)) {
		if (yes_no_prompt
		    ("Do you really want to remove %s? [y/n]: ",
		     lv_name) == 'n') {
			log_print("Logical volume %s not removed", lv_name);
			return 0;
		}
	}

	log_verbose("Releasing logical volume %s", lv_name);
	if (!lv_remove(vg, lvh)) {
		log_error("Error releasing logical volume %s", lv_name);
		return ECMD_FAILED;
	}

/********* FIXME
	log_verbose("Unlinking special file %s", lv_name);
	if (!lvm_check_devfs() && unlink(lv_name) == -1)
		log_error("Error unlinking special file %s", lv_name);
**********/

	/* store it on disks */
	if (ios->vg_write(ios, vg))
		return ECMD_FAILED;

/******** FIXME
	if ((ret = do_autobackup(vg_name, vg)))
		return ret;
**********/

	log_print("logical volume %s successfully removed", lv_name);
	return 0;
}
