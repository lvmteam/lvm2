/*
 * Copyright (C) 2001  Sistina Software
 *
 * vgremove is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * vgremove is distributed in the hope that it will be useful,
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

static int vgremove_single(const char *vg_name);

int vgremove(int argc, char **argv)
{
	return process_each_vg(argc, argv, &vgremove_single);
}

static int vgremove_single(const char *vg_name)
{
	struct volume_group *vg;
	struct physical_volume *pv;
	struct list *pvh;
	int ret = 0;

	log_verbose("Checking for volume group %s", vg_name);
	if (!(vg = fid->ops->vg_read(fid, vg_name))) {
		log_error("Volume group %s doesn't exist", vg_name);
		return ECMD_FAILED;
	}

/******* Ignore active status
	if (vg->status & ACTIVE) {
		log_error("Volume group %s is still active", vg_name);
		return ECMD_FAILED;
	}
********/

	if (vg->lv_count) {
		log_error("Volume group %s still contains %d logical volume(s)",
			  vg_name, vg->lv_count);
		return ECMD_FAILED;
	}

/************ FIXME
	if (vg_remove_dir_and_group_and_nodes(vg_name) < 0) {
		log_error("removing special files of volume group %s",
			  vg_name);
	}
*************/

	/* init physical volumes */
	list_iterate(pvh, &vg->pvs) {
		pv = &list_item(pvh, struct pv_list)->pv;
		log_verbose("Removing physical volume %s from volume group %s",
			    dev_name(pv->dev), vg_name);
		*pv->vg_name = '\0';
		if (!(fid->ops->pv_write(fid, pv))) {
			log_error("Failed to remove physical volume %s from "
				  "volume group %s", dev_name(pv->dev), 
				  vg_name);
			ret = ECMD_FAILED;
		}
	}

	backup(vg);

	if (!ret)
		log_print("Volume group %s successfully removed", vg_name);
	else
		log_error("Volume group %s not properly removed", vg_name);

	return ret;
}
