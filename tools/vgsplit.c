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

static int _move_pv(struct volume_group *vg_from, struct volume_group *vg_to,
		    char *pv_name)
{
	struct pv_list *pvl;
	struct physical_volume *pv;

	if (!(pvl = find_pv_in_vg(vg_from, pv_name))) {
		log_error("Physical volume %s not in volume group %s",
			  pv_name, vg_from->name);
		return 0;
	}

	list_del(&pvl->list);
	list_add(&vg_to->pvs, &pvl->list);

	vg_from->pv_count--;
	vg_to->pv_count++;

	pv = list_item(pvl, struct pv_list)->pv;

	vg_from->extent_count -= pv->pe_count;
	vg_to->extent_count += pv->pe_count;

	vg_from->free_count -= pv->pe_count - pv->pe_alloc_count;
	vg_to->free_count += pv->pe_count - pv->pe_alloc_count;

	return 1;
}

static int _move_lvs(struct volume_group *vg_from, struct volume_group *vg_to)
{
	struct list *lvh, *lvht;
	struct logical_volume *lv;
	struct lv_segment *seg;
	struct physical_volume *pv;
	struct volume_group *vg_with;
	unsigned int s;

	list_iterate_safe(lvh, lvht, &vg_from->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		/* Ensure all the PVs used by this LV remain in the same */
		/* VG as each other */
		vg_with = NULL;
		list_iterate_items(seg, &lv->segments) {
			for (s = 0; s < seg->area_count; s++) {
				/* FIXME Check AREA_LV too */
				if (seg->area[s].type != AREA_PV)
					continue;

				pv = seg->area[s].u.pv.pv;
				if (vg_with) {
					if (!pv_is_in_vg(vg_with, pv)) {
						log_error("Logical Volume %s "
							  "split between "
							  "Volume Groups",
							  lv->name);
						return 0;
					}
					continue;
				}

				if (pv_is_in_vg(vg_from, pv)) {
					vg_with = vg_from;
					continue;
				}
				if (pv_is_in_vg(vg_to, pv)) {
					vg_with = vg_to;
					continue;
				}
				log_error("Physical Volume %s not found",
					  dev_name(pv->dev));
				return 0;
			}
		}

		if (vg_with == vg_from)
			continue;

		/* Move this LV */
		list_del(lvh);
		list_add(&vg_to->lvs, lvh);

		vg_from->lv_count--;
		vg_to->lv_count++;
	}

	/* FIXME Ensure no LVs contain segs pointing at LVs in the other VG */

	return 1;
}

static int _lv_is_in_vg(struct volume_group *vg, struct logical_volume *lv)
{
	struct lv_list *lvl;

	list_iterate_items(lvl, &vg->lvs)
		if (lv == lvl->lv)
			 return 1;

	return 0;
}

static int _move_snapshots(struct volume_group *vg_from,
			   struct volume_group *vg_to)
{
	struct list *slh, *slth;
	struct snapshot *snap;
	int cow_from, origin_from;

	list_iterate_safe(slh, slth, &vg_from->snapshots) {
		snap = list_item(slh, struct snapshot_list)->snapshot;
		cow_from = _lv_is_in_vg(vg_from, snap->cow);
		origin_from = _lv_is_in_vg(vg_from, snap->origin);
		if (cow_from && origin_from)
			return 1;
		if ((!cow_from && origin_from) || (cow_from && !origin_from)) {
			log_error("Snapshot %s split", snap->cow->name);
			return 0;
		}
		vg_from->snapshot_count--;
		vg_to->snapshot_count++;

		list_del(slh);
		list_add(&vg_to->snapshots, slh);
	}

	return 1;
}

int vgsplit(struct cmd_context *cmd, int argc, char **argv)
{
	char *vg_name_from, *vg_name_to;
	struct volume_group *vg_to, *vg_from;
	int opt;
	int active;
	int consistent = 1;

	if (argc < 3) {
		log_error("Existing VG, new VG and physical volumes required.");
		return EINVALID_CMD_LINE;
	}

	vg_name_from = argv[0];
	vg_name_to = argv[1];
	argc -= 2;
	argv += 2;

	if (!strcmp(vg_name_to, vg_name_from)) {
		log_error("Duplicate volume group name \"%s\"", vg_name_from);
		return ECMD_FAILED;
	}

	log_verbose("Checking for volume group \"%s\"", vg_name_from);
	if (!lock_vol(cmd, vg_name_from, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vg_name_from);
		return ECMD_FAILED;
	}

	if (!(vg_from = vg_read(cmd, vg_name_from, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" doesn't exist", vg_name_from);
		unlock_vg(cmd, vg_name_from);
		return ECMD_FAILED;
	}

	if (vg_from->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg_from->name);
		unlock_vg(cmd, vg_name_from);
		return ECMD_FAILED;
	}

	if (!(vg_from->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", vg_from->name);
		unlock_vg(cmd, vg_name_from);
		return ECMD_FAILED;
	}

	log_verbose("Checking for volume group \"%s\"", vg_name_to);
	if (!lock_vol(cmd, vg_name_to, LCK_VG_WRITE | LCK_NONBLOCK)) {
		log_error("Can't get lock for %s", vg_name_to);
		unlock_vg(cmd, vg_name_from);
		return ECMD_FAILED;
	}

	consistent = 0;
	if ((vg_to = vg_read(cmd, vg_name_to, &consistent))) {
		/* FIXME Remove this restriction */
		log_error("Volume group \"%s\" already exists", vg_name_to);
		goto error;
	}

	if ((active = lvs_in_vg_activated(vg_from))) {
		/* FIXME Remove this restriction */
		log_error("Logical volumes in \"%s\" must be inactive",
			  vg_name_from);
		goto error;
	}

	/* Create new VG structure */
	if (!(vg_to = vg_create(cmd, vg_name_to, vg_from->extent_size,
				vg_from->max_pv, vg_from->max_lv, 0, NULL)))
		goto error;

	/* Archive vg_from before changing it */
	if (!archive(vg_from))
		goto error;

	/* Move PVs across to new structure */
	for (opt = 0; opt < argc; opt++) {
		if (!_move_pv(vg_from, vg_to, argv[opt]))
			goto error;
	}

	/* Move required LVs across, checking consistency */
	if (!(_move_lvs(vg_from, vg_to)))
		goto error;

	/* Move required snapshots across */
	if (!(_move_snapshots(vg_from, vg_to)))
		goto error;

	/* FIXME Split mdas properly somehow too! */
	/* Currently we cheat by sharing the format instance and relying on 
	 * vg_write to ignore mdas outside the VG!  Done this way, with text 
	 * format, vg_from disappears for a short time. */
	vg_to->fid = vg_from->fid;

	/* store it on disks */
	log_verbose("Writing out updated volume groups");

	/* Write out new VG as EXPORTED */
	vg_to->status |= EXPORTED_VG;

	if (!archive(vg_to))
		goto error;

	if (!vg_write(vg_to) || !vg_commit(vg_to))
		goto error;

	backup(vg_to);

	/* Write out updated old VG */
	if (!vg_write(vg_from) || !vg_commit(vg_from))
		goto error;

	backup(vg_from);

	/* Remove EXPORTED flag from new VG */
	consistent = 1;
	if (!(vg_to = vg_read(cmd, vg_name_to, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" became inconsistent: please "
			  "fix manually", vg_name_to);
		goto error;
	}

	vg_to->status &= ~EXPORTED_VG;

	if (!vg_write(vg_to) || !vg_commit(vg_to))
		goto error;

	backup(vg_to);

	unlock_vg(cmd, vg_name_from);
	unlock_vg(cmd, vg_name_to);

	log_print("Volume group \"%s\" successfully split from \"%s\"",
		  vg_to->name, vg_from->name);
	return ECMD_PROCESSED;

      error:
	unlock_vg(cmd, vg_name_from);
	unlock_vg(cmd, vg_name_to);
	return ECMD_FAILED;
}
