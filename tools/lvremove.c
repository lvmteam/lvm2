/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved. 
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

static int lvremove_single(struct cmd_context *cmd, struct logical_volume *lv,
			   void *handle)
{
	struct volume_group *vg;
	struct lvinfo info;

	vg = lv->vg;

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", vg->name);
		return ECMD_FAILED;
	}

	if (lv_is_origin(lv)) {
		log_error("Can't remove logical volume \"%s\" under snapshot",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv->status & LOCKED) {
		log_error("Can't remove locked LV %s", lv->name);
		return ECMD_FAILED;
	}

	/* FIXME Ensure not referred to by another existing LVs */

	if (lv_info(lv, &info)) {
		if (info.open_count) {
			log_error("Can't remove open logical volume \"%s\"",
				  lv->name);
			return ECMD_FAILED;
		}

		if (info.exists && !arg_count(cmd, force_ARG)) {
			if (yes_no_prompt("Do you really want to remove active "
					  "logical volume \"%s\"? [y/n]: ",
					  lv->name) == 'n') {
				log_print("Logical volume \"%s\" not removed",
					  lv->name);
				return ECMD_FAILED;
			}
		}
	}

	if (!archive(vg))
		return ECMD_FAILED;

	if (!deactivate_lv(cmd, lv->lvid.s)) {
		log_error("Unable to deactivate logical volume \"%s\"",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv_is_cow(lv)) {
		log_verbose("Removing snapshot %s", lv->name);
		if (!vg_remove_snapshot(lv->vg, lv)) {
			stack;
			return ECMD_FAILED;
		}
	}

	log_verbose("Releasing logical volume \"%s\"", lv->name);
	if (!lv_remove(vg, lv)) {
		log_error("Error releasing logical volume \"%s\"", lv->name);
		return ECMD_FAILED;
	}

	/* store it on disks */
	if (!vg_write(vg))
		return ECMD_FAILED;

	backup(vg);

	if (!vg_commit(vg))
		return ECMD_FAILED;

	log_print("Logical volume \"%s\" successfully removed", lv->name);
	return ECMD_PROCESSED;
}

int lvremove(struct cmd_context *cmd, int argc, char **argv)
{
	if (!argc) {
		log_error("Please enter one or more logical volume paths");
		return EINVALID_CMD_LINE;
	}

	return process_each_lv(cmd, argc, argv, LCK_VG_WRITE, NULL,
			       &lvremove_single);
}
