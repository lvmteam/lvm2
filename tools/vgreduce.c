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

static int _remove_pv(struct volume_group *vg, struct pv_list *pvl)
{
	char uuid[64];

	if (vg->pv_count == 1) {
		log_error("Volume Groups must always contain at least one PV");
		return 0;
	}

	if (!id_write_format(&pvl->pv->id, uuid, sizeof(uuid))) {
		stack;
		return 0;
	}

	log_verbose("Removing PV with UUID %s from VG %s", uuid, vg->name);

	if (pvl->pv->pe_alloc_count) {
		log_error("LVs still present on PV with UUID %s: Can't remove "
			  "from VG %s", uuid, vg->name);
		return 0;
	}

	vg->free_count -= pvl->pv->pe_count;
	vg->extent_count -= pvl->pv->pe_count;
	vg->pv_count--;

	list_del(&pvl->list);

	return 1;
}

static int _remove_lv(struct cmd_context *cmd, struct logical_volume *lv,
		      int *list_unsafe)
{
	struct snapshot *snap;
	struct list *snaplist, *snh;

	log_verbose("%s/%s has missing extents: removing (including "
		    "dependencies)", lv->vg->name, lv->name);

	/* Deactivate if necessary */
	if (!lv_is_cow(lv)) {
		log_verbose("Deactivating (if active) logical volume %s",
			    lv->name);

		if (!lock_vol(cmd, lv->lvid.s,
			      LCK_LV_DEACTIVATE | LCK_NONBLOCK)) {
			log_error("Failed to deactivate LV %s", lv->name);
			return 0;
		}
	} else if ((snap = find_cow(lv))) {
		log_verbose("Deactivating (if active) logical volume %s "
			    "(origin of %s)", snap->origin->name, lv->name);

		if (!lock_vol(cmd, snap->origin->lvid.s,
			      LCK_LV_DEACTIVATE | LCK_NONBLOCK)) {
			log_error("Failed to deactivate LV %s",
				  snap->origin->name);
			return 0;
		}

		/* Use the origin LV */
		lv = snap->origin;
	}

	/* Remove snapshot dependencies */
	if (!(snaplist = find_snapshots(lv))) {
		stack;
		return 0;
	}
	/* List may be empty */
	list_iterate(snh, snaplist) {
		*list_unsafe = 1;	/* May remove caller's lvht! */
		snap = list_item(snh, struct snapshot_list)->snapshot;
		if (!vg_remove_snapshot(lv->vg, snap->cow)) {
			stack;
			return 0;
		}
		log_verbose("Removing LV %s from VG %s", snap->cow->name,
			    lv->vg->name);
		if (!lv_remove(lv->vg, snap->cow)) {
			stack;
			return 0;
		}
	}

	/* Remove the LV itself */
	log_verbose("Removing LV %s from VG %s", lv->name, lv->vg->name);
	if (!lv_remove(lv->vg, lv)) {
		stack;
		return 0;
	}

	return 1;
}

static int _make_vg_consistent(struct cmd_context *cmd, struct volume_group *vg)
{
	struct list *pvh, *pvht;
	struct list *lvh, *lvht;
	struct pv_list *pvl;
	struct logical_volume *lv;
	struct physical_volume *pv;
	struct lv_segment *seg;
	unsigned int s;
	int list_unsafe;
	struct list *segh;

	/* Deactivate & remove necessary LVs */
      restart_loop:
	list_unsafe = 0;	/* Set if we delete a different list-member */

	list_iterate_safe(lvh, lvht, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		/* Are any segments of this LV on missing PVs? */
		list_iterate(segh, &lv->segments) {
			seg = list_item(segh, struct lv_segment);
			for (s = 0; s < seg->area_count; s++) {
				if (seg->area[s].type != AREA_PV)
					continue;

				/* FIXME Also check for segs on deleted LVs */

				pv = seg->area[s].u.pv.pv;
				if (!pv || !pv->dev) {
					if (!_remove_lv(cmd, lv, &list_unsafe)) {
						stack;
						return 0;
					}
					if (list_unsafe)
						goto restart_loop;
				}
			}
		}
	}

	/* Remove missing PVs */
	list_iterate_safe(pvh, pvht, &vg->pvs) {
		pvl = list_item(pvh, struct pv_list);
		if (pvl->pv->dev)
			continue;
		if (!_remove_pv(vg, pvl)) {
			stack;
			return 0;
		}
	}

	return 1;
}

/* Or take pv_name instead? */
static int _vgreduce_single(struct cmd_context *cmd, struct volume_group *vg,
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

	if (!vg_write(vg) || !vg_commit(vg)) {
		log_error("Removal of physical volume \"%s\" from "
			  "\"%s\" failed", name, vg->name);
		return ECMD_FAILED;
	}

	if (!pv_write(cmd, pv, NULL, INT64_C(-1))) {
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
	int ret = 1;
	int consistent = 1;

	if (!argc & !arg_count(cmd, removemissing_ARG)) {
		log_error("Please give volume group name and "
			  "physical volume paths");
		return EINVALID_CMD_LINE;
	}

	if (!argc & arg_count(cmd, removemissing_ARG)) {
		log_error("Please give volume group name");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1 && !arg_count(cmd, all_ARG)
	    && !arg_count(cmd, removemissing_ARG)) {
		log_error("Please enter physical volume paths or option -a");
		return EINVALID_CMD_LINE;
	}

	if (argc > 1 && arg_count(cmd, all_ARG)) {
		log_error("Option -a and physical volume paths mutually "
			  "exclusive");
		return EINVALID_CMD_LINE;
	}

	if (argc > 1 && arg_count(cmd, removemissing_ARG)) {
		log_error("Please only specify the volume group");
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

	if ((!(vg = vg_read(cmd, vg_name, &consistent)) || !consistent) &&
	    !arg_count(cmd, removemissing_ARG)) {
		log_error("Volume group \"%s\" doesn't exist", vg_name);
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}

	if (arg_count(cmd, removemissing_ARG)) {
		if (vg && consistent) {
			log_error("Volume group \"%s\" is already consistent",
				  vg_name);
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}

		init_partial(1);
		consistent = 0;
		if (!(vg = vg_read(cmd, vg_name, &consistent))) {
			log_error("Volume group \"%s\" not found", vg_name);
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}
		if (!archive(vg)) {
			init_partial(0);
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}

		if (!_make_vg_consistent(cmd, vg)) {
			init_partial(0);
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}

		init_partial(0);

		vg->status &= ~PARTIAL_VG;
		vg->status |= LVM_WRITE;

		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Failed to write out a consistent VG for %s",
				  vg_name);
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}

		backup(vg);

		log_print("Wrote out consistent volume group %s", vg_name);

	} else {
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
			log_error("Volume group \"%s\" is not reducible",
				  vg_name);
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}

		/* FIXME: Pass private struct through to all these functions */
		/* and update in batch here? */
		ret = process_each_pv(cmd, argc, argv, vg, NULL,
				      _vgreduce_single);

	}

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
