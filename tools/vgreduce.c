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

/* Or take pv_name instead? */
static int vgreduce_single(struct cmd_context *cmd, struct volume_group *vg,
			   struct physical_volume *pv, void *handle)
{
	struct pv_list *pvl;
	const char *name = dev_name(pv->dev);

	if (pv->pe_alloc_count) {
		log_error("Physical volume \"%s\" still in use", name);
		return ECMD_FAILED;
	}

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

	pv->vg_name = ORPHAN;
	vg->pv_count--;
	vg->free_count -= pv->pe_count - pv->pe_alloc_count;
	vg->extent_count -= pv->pe_count;

	if (!vg_write(vg)) {
		log_error("Removal of physical volume \"%s\" from "
			  "\"%s\" failed", name, vg->name);
		return ECMD_FAILED;
	}

	if (!pv_write(cmd, pv, NULL, __INT64_C(-1))) {
		log_error("Failed to clear metadata from physical "
			  "volume \"%s\" "
			  "after removal from \"%s\"", name, vg->name);
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Removed \"%s\" from volume group \"%s\"", name, vg->name);

	return 0;
}
int vgreduce(struct cmd_context *cmd, int argc, char **argv)
{
	struct volume_group *vg;
	char *vg_name;
	int ret;
	int consistent = 1;

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

	if (!(vg = vg_read(cmd, vg_name, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" doesn't exist", vg_name);
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg->name);
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", vg_name);
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}

	if (!(vg->status & RESIZEABLE_VG)) {
		log_error("Volume group \"%s\" is not reducable", vg_name);
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}

	/* FIXME: Pass private structure through to all these functions */
	/* and update in batch here? */
	ret = process_each_pv(cmd, argc, argv, vg, NULL, vgreduce_single);

	unlock_vg(cmd, vg_name);

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
