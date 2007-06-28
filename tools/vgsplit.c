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

	vg_from->extent_count -= pv_pe_count(pv);
	vg_to->extent_count += pv_pe_count(pv);

	vg_from->free_count -= pv_pe_count(pv) - pv_pe_alloc_count(pv);
	vg_to->free_count += pv_pe_count(pv) - pv_pe_alloc_count(pv);

	return 1;
}

/* FIXME Why not (lv->vg == vg) ? */
static int _lv_is_in_vg(struct volume_group *vg, struct logical_volume *lv)
{
	struct lv_list *lvl;

	list_iterate_items(lvl, &vg->lvs)
		if (lv == lvl->lv)
			 return 1;

	return 0;
}


static int _move_lvs(struct volume_group *vg_from, struct volume_group *vg_to)
{
	struct list *lvh, *lvht;
	struct logical_volume *lv;
	struct lv_segment *seg;
	struct physical_volume *pv;
	struct volume_group *vg_with;
	unsigned s;

	list_iterate_safe(lvh, lvht, &vg_from->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		if ((lv->status & SNAPSHOT))
			continue;

		if ((lv->status & MIRRORED))
			continue;

		/* Ensure all the PVs used by this LV remain in the same */
		/* VG as each other */
		vg_with = NULL;
		list_iterate_items(seg, &lv->segments) {
			for (s = 0; s < seg->area_count; s++) {
				/* FIXME Check AREA_LV too */
				if (seg_type(seg, s) != AREA_PV)
					continue;

				pv = seg_pv(seg, s);
				if (vg_with) {
					if (!pv_is_in_vg(vg_with, pv)) {
						log_error("Can't split Logical "
							  "Volume %s between "
							  "two Volume Groups",
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
					  dev_name(pv_dev(pv)));
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

static int _move_snapshots(struct volume_group *vg_from,
			   struct volume_group *vg_to)
{
	struct list *lvh, *lvht;
	struct logical_volume *lv;
	struct lv_segment *seg;
	int cow_from = 0;
	int origin_from = 0;

	list_iterate_safe(lvh, lvht, &vg_from->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		if (!(lv->status & SNAPSHOT))
			continue;

		list_iterate_items(seg, &lv->segments) {
			cow_from = _lv_is_in_vg(vg_from, seg->cow);
			origin_from = _lv_is_in_vg(vg_from, seg->origin);

			if (cow_from && origin_from)
				continue;
			if ((!cow_from && origin_from) ||
			     (cow_from && !origin_from)) {
				log_error("Can't split snapshot %s between"
					  " two Volume Groups", seg->cow->name);
				return 0;
			}
		}

		/* Move this snapshot */
		list_del(lvh);
		list_add(&vg_to->lvs, lvh);

		vg_from->snapshot_count--;
		vg_to->snapshot_count++;
	}

	return 1;
}

static int _move_mirrors(struct volume_group *vg_from,
			 struct volume_group *vg_to)
{
	struct list *lvh, *lvht;
	struct logical_volume *lv;
	struct lv_segment *seg;
	int i, seg_in, log_in;

	list_iterate_safe(lvh, lvht, &vg_from->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		if (!(lv->status & MIRRORED))
			continue;

		seg = first_seg(lv); 

		seg_in = 0;
		for (i = 0; i < seg->area_count; i++)
			if (_lv_is_in_vg(vg_to, seg_lv(seg, i)))
			    seg_in++;

		log_in = (!seg->log_lv || _lv_is_in_vg(vg_to, seg->log_lv));
		
		if ((seg_in && seg_in < seg->area_count) || 
		    (seg_in && seg->log_lv && !log_in) || 
		    (!seg_in && seg->log_lv && log_in)) {
			log_error("Can't split mirror %s between "
				  "two Volume Groups", lv->name);
			return 0;
		}

		if (seg_in == seg->area_count && log_in) {
			list_del(lvh);
			list_add(&vg_to->lvs, lvh);

			vg_from->lv_count--;
			vg_to->lv_count++;
		}
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

	vg_name_from = skip_dev_dir(cmd, argv[0], NULL);
	vg_name_to = skip_dev_dir(cmd, argv[1], NULL);
	argc -= 2;
	argv += 2;

	if (!validate_name(vg_name_from)) {
		log_error("Volume group name \"%s\" is invalid",
			  vg_name_from);
		return ECMD_FAILED;
	}

	if (!strcmp(vg_name_to, vg_name_from)) {
		log_error("Duplicate volume group name \"%s\"", vg_name_from);
		return ECMD_FAILED;
	}

	log_verbose("Checking for volume group \"%s\"", vg_name_from);
	if (!lock_vol(cmd, vg_name_from, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vg_name_from);
		return ECMD_FAILED;
	}

	if (!(vg_from = vg_read(cmd, vg_name_from, NULL, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" doesn't exist", vg_name_from);
		unlock_vg(cmd, vg_name_from);
		return ECMD_FAILED;
	}

	if (!vg_check_status(vg_from, CLUSTERED | EXPORTED_VG |
				      RESIZEABLE_VG | LVM_WRITE)) {
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
	if ((vg_to = vg_read(cmd, vg_name_to, NULL, &consistent))) {
		/* FIXME Remove this restriction */
		log_error("Volume group \"%s\" already exists", vg_name_to);
		goto error;
	}

	if (!validate_vg_name(cmd, vg_name_to)) {
		log_error("New volume group name \"%s\" is invalid",
			   vg_name_to);
		goto error;
	}

	if ((active = lvs_in_vg_activated(vg_from))) {
		/* FIXME Remove this restriction */
		log_error("Logical volumes in \"%s\" must be inactive",
			  vg_name_from);
		goto error;
	}

	/* Set metadata format of original VG */
	/* FIXME: need some common logic */
	cmd->fmt = vg_from->fid->fmt;

	/* Create new VG structure */
	if (!(vg_to = vg_create(cmd, vg_name_to, vg_from->extent_size,
				vg_from->max_pv, vg_from->max_lv,
				vg_from->alloc, 0, NULL)))
		goto error;

	if (vg_from->status & CLUSTERED)
		vg_to->status |= CLUSTERED;

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

	/* Move required mirrors across */
	if (!(_move_mirrors(vg_from, vg_to)))
		goto error;

	/* Split metadata areas and check if both vgs have at least one area */
	if (!(vg_split_mdas(cmd, vg_from, vg_to)) && vg_from->pv_count) {
		log_error("Cannot split: Nowhere to store metadata for new Volume Group");
		goto error;
	}

	/* Set proper name for all PVs in new VG */
	if (!vg_rename(cmd, vg_to, vg_name_to))
		goto error;

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
	if (vg_from->pv_count) {
		if (!vg_write(vg_from) || !vg_commit(vg_from))
			goto error;

		backup(vg_from);
	}

	/* Remove EXPORTED flag from new VG */
	consistent = 1;
	if (!(vg_to = vg_read(cmd, vg_name_to, NULL, &consistent)) || !consistent) {
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
