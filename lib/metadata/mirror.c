/*
 * Copyright (C) 2003  Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "metadata.h"
#include "toolcontext.h"

/* 
 * Replace any LV segments on given PV with temporary mirror.
 * Returns list of LVs changed.
 */
int insert_pvmove_mirrors(struct cmd_context *cmd,
			  struct logical_volume *lv_mirr,
			  struct physical_volume *pv,
			  struct logical_volume *lv,
			  struct list *allocatable_pvs,
			  struct list *lvs_changed)
{
	struct list *segh;
	struct lv_segment *seg;
	struct lv_list *lvl;
	int lv_used = 0;
	uint32_t s, start_le, extent_count = 0u;

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct lv_segment);
		for (s = 0; s < seg->area_count; s++) {
			if (seg->area[s].type != AREA_PV ||
			    seg->area[s].u.pv.pv->dev != pv->dev)
				continue;

			if (!lv_used) {
				if (!(lvl = pool_alloc(cmd->mem, sizeof(*lvl)))) {
					log_error("lv_list alloc failed");
					return 0;
				}
				lvl->lv = lv;
				list_add(lvs_changed, &lvl->list);
				lv_used = 1;
			}

			start_le = lv_mirr->le_count;
			if (!lv_extend_mirror(lv->vg->fid, lv_mirr,
					      seg->area[s].u.pv.pv,
					      seg->area[s].u.pv.pe,
					      seg->area_len, allocatable_pvs,
					      PVMOVE)) {
				log_error("Allocation for temporary "
					  "pvmove LV failed");
				return 0;
			}
			seg->area[s].type = AREA_LV;
			seg->area[s].u.lv.lv = lv_mirr;
			seg->area[s].u.lv.le = start_le;

			extent_count += seg->area_len;

			lv->status |= LOCKED;
		}
	}

	log_verbose("Moving %u extents of logical volume %s/%s", extent_count,
		    lv->vg->name, lv->name);

	return 1;
}

/* Remove a temporary mirror */
int remove_pvmove_mirrors(struct volume_group *vg,
			  struct logical_volume *lv_mirr)
{
	struct list *lvh, *segh;
	struct logical_volume *lv1;
	struct lv_segment *seg, *mir_seg;
	uint32_t s, c;

	list_iterate(lvh, &vg->lvs) {
		lv1 = list_item(lvh, struct lv_list)->lv;
		if (lv1 == lv_mirr)
			continue;

		list_iterate(segh, &lv1->segments) {
			seg = list_item(segh, struct lv_segment);
			for (s = 0; s < seg->area_count; s++) {
				if (seg->area[s].type != AREA_LV ||
				    seg->area[s].u.lv.lv != lv_mirr)
					continue;

				if (!(mir_seg = find_seg_by_le(lv_mirr,
							       seg->area[s].u.
							       lv.le))) {
					log_error("No segment found with LE");
					return 0;
				}

				if (mir_seg->type != SEG_MIRRORED ||
				    !(mir_seg->status & PVMOVE) ||
				    mir_seg->le != seg->area[s].u.lv.le ||
				    mir_seg->area_count != 2 ||
				    mir_seg->area_len != seg->area_len) {
					log_error("Incompatible segments");
					return 0;
				}

				if (mir_seg->extents_moved == mir_seg->area_len)
					c = 1;
				else
					c = 0;

				seg->area[s].type = AREA_PV;
				seg->area[s].u.pv.pv = mir_seg->area[c].u.pv.pv;
				seg->area[s].u.pv.pe = mir_seg->area[c].u.pv.pe;

				mir_seg->type = SEG_STRIPED;
				mir_seg->area_count = 1;

				lv1->status &= ~LOCKED;
			}
		}
	}

	return 1;
}

struct physical_volume *get_pvmove_pv_from_lv_mirr(struct logical_volume
						   *lv_mirr)
{
	struct list *segh;
	struct lv_segment *seg;

	list_iterate(segh, &lv_mirr->segments) {
		seg = list_item(segh, struct lv_segment);
		if (seg->type != SEG_MIRRORED)
			continue;
		if (seg->area[0].type != AREA_PV)
			continue;
		return seg->area[0].u.pv.pv;
	}

	return NULL;
}

struct physical_volume *get_pvmove_pv_from_lv(struct logical_volume *lv)
{
	struct list *segh;
	struct lv_segment *seg;
	uint32_t s;

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct lv_segment);
		for (s = 0; s < seg->area_count; s++) {
			if (seg->area[s].type != AREA_LV)
				continue;
			return get_pvmove_pv_from_lv_mirr(seg->area[s].u.lv.lv);
		}
	}

	return NULL;
}

struct logical_volume *find_pvmove_lv(struct volume_group *vg,
				      struct device *dev)
{
	struct list *lvh, *segh;
	struct logical_volume *lv;
	struct lv_segment *seg;

	/* Loop through all LVs */
	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		if (!(lv->status & PVMOVE))
			continue;

		list_iterate(segh, &lv->segments) {
			seg = list_item(segh, struct lv_segment);
			if (seg->area[0].type != AREA_PV)
				continue;
			if (seg->area[0].u.pv.pv->dev != dev)
				continue;
			return lv;
		}
	}

	return NULL;
}

struct list *lvs_using_lv(struct cmd_context *cmd, struct volume_group *vg,
			  struct logical_volume *lv)
{
	struct list *lvh, *segh, *lvs;
	struct logical_volume *lv1;
	struct lv_list *lvl;
	struct lv_segment *seg;
	uint32_t s;

	if (!(lvs = pool_alloc(cmd->mem, sizeof(*lvs)))) {
		log_error("lvs list alloc failed");
		return NULL;
	}

	list_init(lvs);

	/* Loop through all LVs except the one supplied */
	list_iterate(lvh, &vg->lvs) {
		lv1 = list_item(lvh, struct lv_list)->lv;
		if (lv1 == lv)
			continue;

		list_iterate(segh, &lv1->segments) {
			seg = list_item(segh, struct lv_segment);
			for (s = 0; s < seg->area_count; s++) {
				if (seg->area[s].type != AREA_LV ||
				    seg->area[s].u.lv.lv != lv)
					continue;
				if (!(lvl = pool_alloc(cmd->mem, sizeof(*lvl)))) {
					log_error("lv_list alloc failed");
					return NULL;
				}
				lvl->lv = lv1;
				list_add(lvs, &lvl->list);
				goto next_lv;
			}
		}
	      next_lv:
	}

	return lvs;
}

float pvmove_percent(struct logical_volume *lv_mirr)
{
	uint32_t numerator = 0u, denominator = 0u;
	struct list *segh;
	struct lv_segment *seg;

	list_iterate(segh, &lv_mirr->segments) {
		seg = list_item(segh, struct lv_segment);
		if (!(seg->status & PVMOVE))
			continue;

		numerator += seg->extents_moved;
		denominator += seg->area_len;
	}

	return denominator ? (float) numerator *100 / denominator : 100.0;
}
