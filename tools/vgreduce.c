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

static int vgreduce_single(struct cmd_context *cmd, struct volume_group *vg,
			   struct physical_volume *pv);

int vgreduce(struct cmd_context *cmd, int argc, char **argv)
{
	struct volume_group *vg;
	char *vg_name;
	int ret;

	if (!argc) {
		log_error("Please give volume group name and "
			  "physical volume paths");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1 && !arg_count(cmd, all_ARG)) {
		log_error("Please enter physical volume paths or option -a");
		return EINVALID_CMD_LINE;
	}

	if (argc > 1 && arg_count(cmd, all_ARG)) {
		log_error("Option -a and physical volume paths mutually "
			  "exclusive");
		return EINVALID_CMD_LINE;
	}

	vg_name = argv[0];
	argv++;
	argc--;

	log_verbose("Finding volume group \"%s\"", vg_name);
	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg = cmd->fid->ops->vg_read(cmd->fid, vg_name))) {
		log_error("Volume group \"%s\" doesn't exist", vg_name);
		lock_vol(cmd, vg_name, LCK_VG_UNLOCK);
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg->name);
		lock_vol(cmd, vg_name, LCK_VG_UNLOCK);
		return ECMD_FAILED;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", vg_name);
		lock_vol(cmd, vg_name, LCK_VG_UNLOCK);
		return ECMD_FAILED;
	}

	if (!(vg->status & RESIZEABLE_VG)) {
		log_error("Volume group \"%s\" is not reducable", vg_name);
		lock_vol(cmd, vg_name, LCK_VG_UNLOCK);
		return ECMD_FAILED;
	}

	/* FIXME: Pass private structure through to all these functions */
	/* and update in batch here? */
	ret = process_each_pv(cmd, argc, argv, vg, vgreduce_single);

	lock_vol(cmd, vg_name, LCK_VG_UNLOCK);

	return ret;

/******* FIXME
	log_error ("no empty physical volumes found in volume group \"%s\"", vg_name);

	log_verbose
	    ("volume group \"%s\" will be reduced by %d physical volume%s",
	     vg_name, np, np > 1 ? "s" : "");
	log_verbose ("reducing volume group \"%s\" by physical volume \"%s\"",
		     vg_name, pv_names[p]);

	log_print
	    ("volume group \"%s\" %ssuccessfully reduced by physical volume%s:",
	     vg_name, error > 0 ? "NOT " : "", p > 1 ? "s" : "");
		log_print("%s", pv_this[p]->pv_name);
********/

}

/* Or take pv_name instead? */
static int vgreduce_single(struct cmd_context *cmd, struct volume_group *vg,
			   struct physical_volume *pv)
{
	struct pv_list *pvl;
	const char *name = dev_name(pv->dev);

	if (pv->pe_allocated) {
		log_error("Physical volume \"%s\" still in use", name);
		return ECMD_FAILED;
	}

/********* FIXME: Is this unnecessary after checking pe_allocated?
	if (pv->lv_cur > 0) {
		log_error ("can't reduce volume group \"%s\" by used physical volume \"%s\"", vg_name, error_pv_name);
	}
*********/

	if (vg->pv_count == 1) {
		log_error("Can't remove final physical volume \"%s\" from "
			  "volume group \"%s\"", name, vg->name);
		return ECMD_FAILED;
	}

	pvl = find_pv_in_vg(vg, name);

	if (!archive(vg))
		return ECMD_FAILED;

	log_verbose("Removing \"%s\" from volume group \"%s\"", name, vg->name);

	if (pvl)
		list_del(&pvl->list);

	*pv->vg_name = '\0';
	vg->pv_count--;
	vg->free_count -= pv->pe_count - pv->pe_allocated;
	vg->extent_count -= pv->pe_count;

	if (!(cmd->fid->ops->vg_write(cmd->fid, vg))) {
		log_error("Removal of physical volume \"%s\" from "
			  "\"%s\" failed", name, vg->name);
		return ECMD_FAILED;
	}

	if (!cmd->fid->ops->pv_write(cmd->fid, pv)) {
		log_error("Failed to clear metadata from physical "
			  "volume \"%s\" "
			  "after removal from \"%s\"", name, vg->name);
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Removed \"%s\" from volume group \"%s\"", name, vg->name);

	return 0;
}
