/*
 * Copyright (C) 2003  Sistina Software
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

/* Allow /dev/vgname/lvname, vgname/lvname or lvname */
static const char *_extract_lvname(struct cmd_context *cmd, const char *vgname,
				   const char *arg)
{
	const char *lvname;

	/* Is an lvname supplied directly? */
	if (!strchr(arg, '/'))
		return arg;

	lvname = arg;
	if (!strncmp(lvname, cmd->dev_dir, strlen(cmd->dev_dir)))
		lvname += strlen(cmd->dev_dir);
	while (*lvname == '/')
		lvname++;
	if (!strchr(lvname, '/')) {
		log_error("--name takes a logical volume name");
		return NULL;
	}
	if (strncmp(vgname, lvname, strlen(vgname)) ||
	    (lvname += strlen(vgname), *lvname != '/')) {
		log_error("Named LV and old PV must be in the same VG");
		return NULL;
	}
	while (*lvname == '/')
		lvname++;
	if (!*lvname) {
		log_error("Incomplete LV name supplied with --name");
		return NULL;
	}
	return lvname;
}

int pvmove(struct cmd_context *cmd, int argc, char **argv)
{
	char *pv_name;
	const char *lv_name = NULL;
	struct volume_group *vg;
	struct list *allocatable_pvs, *pvht, *pvh, *lvh;
	struct list lvs_changed;
	struct pv_list *pvl;
	struct physical_volume *pv;
	struct logical_volume *lv, *lv_mirr;
	int consistent = 1;
	int success = 0;
	int interval;
	float percent = 0.0;
	uint32_t event_nr = 0;

	if (!argc) {
		log_error("Old physical volume name needs specifying");
		return EINVALID_CMD_LINE;
	}

	pv_name = argv[0];
	argc--;
	argv++;

	/* Find VG containing the PV */
	if (!(pv = pv_read(cmd, pv_name, NULL, NULL))) {
		log_error("Physical volume %s not found", pv_name);
		return EINVALID_CMD_LINE;
	}

	if (!pv->vg_name[0]) {
		log_error("Physical volume %s not in a volume group", pv_name);
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, name_ARG)) {
		if (!(lv_name = _extract_lvname(cmd, pv->vg_name,
						arg_value(cmd, name_ARG)))) {
			stack;
			return EINVALID_CMD_LINE;
		}
	}

	/* Read VG */
	log_verbose("Finding volume group \"%s\"", pv->vg_name);

	if (!lock_vol(cmd, pv->vg_name, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", pv->vg_name);
		return ECMD_FAILED;
	}

	if (!(vg = vg_read(cmd, pv->vg_name, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" doesn't exist", pv->vg_name);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", pv->vg_name);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", pv->vg_name);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	/* Create list of PVs for allocation of replacement extents */
	if (argc) {
		if (!(allocatable_pvs = create_pv_list(cmd->mem, vg, argc,
						       argv))) {
			stack;
			unlock_vg(cmd, pv->vg_name);
			return ECMD_FAILED;
		}
	} else {
		if (!(allocatable_pvs = clone_pv_list(cmd->mem, &vg->pvs))) {
			stack;
			unlock_vg(cmd, pv->vg_name);
			return ECMD_FAILED;
		}
	}

	/* Don't allocate onto the PV we're clearing! */
	/* Remove PV if full */
	list_iterate_safe(pvh, pvht, allocatable_pvs) {
		pvl = list_item(pvh, struct pv_list);
		if ((pvl->pv->dev == pv->dev) ||
		    (pvl->pv->pe_count == pvl->pv->pe_alloc_count))
			list_del(&pvl->list);
	}

	if (list_empty(allocatable_pvs)) {
		log_error("No extents available for allocation");
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (!archive(vg)) {
		unlock_vg(cmd, pv->vg_name);
		stack;
		return ECMD_FAILED;
	}

	if (!(lv_mirr = lv_create_empty(vg->fid, NULL, LVM_READ | LVM_WRITE,
					ALLOC_CONTIGUOUS, vg))) {
		log_error("Creation of temporary pvmove LV failed");
		return ECMD_FAILED;
	}

	list_init(&lvs_changed);

	/* Find segments to be moved and set up mirrors */
	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		if ((lv == lv_mirr) || (lv_name && strcmp(lv->name, lv_name)))
			continue;
		if (lv_is_origin(lv) || lv_is_cow(lv)) {
			log_print("Skipping snapshot-related LV %s", lv->name);
			continue;
		}
		if (!insert_pvmove_mirrors(cmd, lv_mirr, pv, lv,
					   allocatable_pvs, &lvs_changed)) {
			stack;
			return ECMD_FAILED;
		}
	}

	if (!lv_mirr->le_count) {
		log_error("No data to move for %s", vg->name);
		return ECMD_FAILED;
	}

	if (!lock_lvs(cmd, &lvs_changed, LCK_LV_ACTIVATE)) {
		stack;
		unlock_lvs(cmd, &lvs_changed);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (!lock_lvs(cmd, &lvs_changed, LCK_LV_SUSPEND | LCK_HOLD)) {
		stack;
		unlock_lvs(cmd, &lvs_changed);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	init_pvmove(1);

	vg->status |= PVMOVE_VG;

	log_verbose("Writing out volume group with temporary pvmove LV");
	if (!vg_write(vg)) {
		log_error("ABORTING: You might need to use vgcfgrestore "
			  "to recover.");
		unlock_lvs(cmd, &lvs_changed);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (!lock_vol(cmd, lv_mirr->lvid.s, LCK_LV_ACTIVATE)) {
		log_error("ABORTING: Mirror activation failed. "
			  "Restoring metadata.");
		unlock_lvs(cmd, &lvs_changed);
		goto clearup;
	}

	unlock_lvs(cmd, &lvs_changed);

	interval = arg_int_value(cmd, interval_ARG, DEFAULT_INTERVAL);

	/* Poll for mirror completion */
	while (percent < 100.0) {
		if (interval)
			sleep(interval);
		if (!lv_mirror_percent(lv_mirr, !interval, &percent,
				       &event_nr)) {
			log_error("ABORTING: Mirror percentage check failed. "
				  "Restoring metadata.");
			break;
		}
		if (interval)
			log_print("Mirror percent: %f", percent);
		else
			log_verbose("Mirror percent: %f", percent);
	}

	if (percent >= 100.0)
		success = 1;

	if (!lock_lvs(cmd, &lvs_changed, LCK_LV_SUSPEND | LCK_HOLD)) {
		init_pvmove(0);
		unlock_lvs(cmd, &lvs_changed);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (!unlock_lv(cmd, lv_mirr->lvid.s)) {
		log_error("Unable to unlock logical volume \"%s\"",
			  lv_mirr->name);
	}

      clearup:
	if (!lock_vol(cmd, lv_mirr->lvid.s, LCK_LV_DEACTIVATE)) {
		log_error("ABORTING: Unable to deactivate temporary logical "
			  "volume \"%s\"", lv_mirr->name);
		log_error("You might need to use vgcfgrestore to recover.");
		unlock_lvs(cmd, &lvs_changed);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (!remove_pvmove_mirrors(cmd, vg, lv_mirr, success)) {
		log_error("ABORTING: Temporary mirror removal failed");
		log_error("You might need to use vgcfgrestore to recover.");
		unlock_lvs(cmd, &lvs_changed);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	if (!lv_remove(vg, lv_mirr)) {
		log_error("ABORTING: Removal of temporary pvmove LV failed");
		log_error("You might need to use vgcfgrestore to recover.");
		unlock_lvs(cmd, &lvs_changed);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	vg->status &= ~PVMOVE_VG;
	init_pvmove(0);

	log_verbose("Writing out final volume group after pvmove");
	if (!vg_write(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		log_error("You might need to use vgcfgrestore to recover.");
		unlock_lvs(cmd, &lvs_changed);
		unlock_vg(cmd, pv->vg_name);
		return ECMD_FAILED;
	}

	unlock_lvs(cmd, &lvs_changed);

	backup(vg);

	unlock_vg(cmd, pv->vg_name);

	return success ? ECMD_PROCESSED : ECMD_FAILED;
}
