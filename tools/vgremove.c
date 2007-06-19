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

static int vgremove_single(struct cmd_context *cmd, const char *vg_name,
			   struct volume_group *vg, int consistent,
			   void *handle __attribute((unused)))
{
	struct physical_volume *pv;
	struct pv_list *pvl;
	int ret = ECMD_PROCESSED;

	if (!vg || !consistent || (vg_status(vg) & PARTIAL_VG)) {
		log_error("Volume group \"%s\" not found or inconsistent.",
			  vg_name);
		log_error("Consider vgreduce --removemissing if metadata "
			  "is inconsistent.");
		return ECMD_FAILED;
	}

	if (!vg_check_status(vg, EXPORTED_VG))
		return ECMD_FAILED;

	if (vg->lv_count) {
		log_error("Volume group \"%s\" still contains %d "
			  "logical volume(s)", vg_name, vg->lv_count);
		return ECMD_FAILED;
	}

	if (!archive(vg))
		return ECMD_FAILED;

	if (!vg_remove(vg)) {
		log_error("vg_remove %s failed", vg_name);
		return ECMD_FAILED;
	}

	/* init physical volumes */
	list_iterate_items(pvl, &vg->pvs) {
		pv = pvl->pv;
		log_verbose("Removing physical volume \"%s\" from "
			    "volume group \"%s\"", dev_name(pv_dev(pv)), vg_name);
		pv->vg_name = ORPHAN;
		pv->status = ALLOCATABLE_PV;

		if (!dev_get_size(pv_dev(pv), &pv->size)) {
			log_error("%s: Couldn't get size.", dev_name(pv_dev(pv)));
			ret = ECMD_FAILED;
			continue;
		}

		/* FIXME Write to same sector label was read from */
		if (!pv_write(cmd, pv, NULL, INT64_C(-1))) {
			log_error("Failed to remove physical volume \"%s\""
				  " from volume group \"%s\"",
				  dev_name(pv_dev(pv)), vg_name);
			ret = ECMD_FAILED;
		}
	}

	backup_remove(cmd, vg_name);

	if (ret == ECMD_PROCESSED)
		log_print("Volume group \"%s\" successfully removed", vg_name);
	else
		log_error("Volume group \"%s\" not properly removed", vg_name);

	return ret;
}

int vgremove(struct cmd_context *cmd, int argc, char **argv)
{
	int ret;

	if (!argc) {
		log_error("Please enter one or more volume group paths");
		return EINVALID_CMD_LINE;
	}

	if (!lock_vol(cmd, ORPHAN, LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return ECMD_FAILED;
	}

	ret = process_each_vg(cmd, argc, argv,
			      LCK_VG_WRITE | LCK_NONBLOCK, 1, 
			      NULL, &vgremove_single);

	unlock_vg(cmd, ORPHAN);

	return ret;
}
