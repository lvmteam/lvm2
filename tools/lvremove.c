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

static int lvremove_single(struct logical_volume *lv);

int lvremove(int argc, char **argv)
{
	if (!argc) {
		log_error("Please enter one or more logical volume paths");
		return EINVALID_CMD_LINE;
	}

	return process_each_lv(argc, argv, &lvremove_single);
}

static int lvremove_single(struct logical_volume *lv)
{
	struct volume_group *vg;

	vg = lv->vg;
	if (!(vg->status & ACTIVE)) {
		log_error("Volume group %s must be active before removing a "
			  "logical volume", vg->name);
		return ECMD_FAILED;
	}

	if (lv->status & SNAPSHOT_ORG) {
		log_error("Can't remove logical volume %s under snapshot",
			  lv->name);
		return ECMD_FAILED;
	}

/********** FIXME  Ensure logical volume is not open on *any* machine
	if (lv->open) {
		log_error("can't remove open %s logical volume %s",
			  lv->status & SNAPSHOT ? "snapshot" : "",
			  lv->name);
		return ECMD_FAILED;
	}
************/

	if (!arg_count(force_ARG)) {
		if (yes_no_prompt
		    ("Do you really want to remove %s? [y/n]: ",
		     lv->name) == 'n') {
			log_print("Logical volume %s not removed", lv->name);
			return 0;
		}
	}

	log_verbose("Releasing logical volume %s", lv->name);
	if (!lv_remove(vg, lv)) {
		log_error("Error releasing logical volume %s", lv->name);
		return ECMD_FAILED;
	}

/********* FIXME
	log_verbose("Unlinking special file %s", lv->name);
	if (!lvm_check_devfs() && unlink(lv->name) == -1)
		log_error("Error unlinking special file %s", lv_name);
**********/

	/* store it on disks */
	if (fid->ops->vg_write(fid, vg))
		return ECMD_FAILED;

/******** FIXME
	if ((ret = do_autobackup(vg->name, vg)))
		return ret;
**********/

	log_print("logical volume %s successfully removed", lv->name);
	return 0;
}
