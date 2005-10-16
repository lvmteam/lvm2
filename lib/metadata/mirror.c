/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.  
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

#include "lib.h"
#include "metadata.h"
#include "toolcontext.h"
#include "segtype.h"
#include "display.h"
#include "activate.h"
#include "lv_alloc.h"
#include "lvm-string.h"

/*
 * Ensure region size is compatible with volume size.
 */
uint32_t adjusted_mirror_region_size(uint32_t extent_size, uint32_t extents,
				     uint32_t region_size)
{
	uint32_t region_max;

	region_max = (1 << (ffs(extents) - 1)) * extent_size;

	if (region_max < region_size) {
		region_size = region_max;
		log_print("Using reduced mirror region size of %" PRIu32
			  " sectors", region_max);
		return region_max;
	}

	return region_size;
}

/*
 * Reduce mirrored_seg to num_mirrors images.
 */
int remove_mirror_images(struct lv_segment *mirrored_seg, uint32_t num_mirrors)
{
	uint32_t m;

	for (m = num_mirrors; m < mirrored_seg->area_count; m++) {
		if (!lv_remove(seg_lv(mirrored_seg, m))) {
			stack;
			return 0;
		}
	}

	mirrored_seg->area_count = num_mirrors;

	return 1;
}

int remove_all_mirror_images(struct logical_volume *lv)
{
	struct lv_segment *first_seg, *seg;
	struct logical_volume *lv1;

	list_iterate_items(first_seg, &lv->segments)
		break;

	if (!remove_mirror_images(first_seg, 1)) {
		stack;
		return 0;
	}

	if (!lv_remove(first_seg->log_lv)) {
		stack;
		return 0;
	}

	lv1 = seg_lv(first_seg, 0);

	lv->segments = lv1->segments;
	lv->segments.n->p = &lv->segments;
	lv->segments.p->n = &lv->segments;

	list_init(&lv1->segments);
	lv1->le_count = 0;
	lv1->size = 0;
	if (!lv_remove(lv1)) {
		stack;
		return 0;
	}

	lv->status &= ~MIRRORED;

	list_iterate_items(seg, &lv->segments)
		seg->lv = lv;

	return 1;
}

/*
 * Add mirror images to an existing mirror
 */
/* FIXME
int add_mirror_images(struct alloc_handle *ah,
		      uint32_t first_area,
		      uint32_t num_areas,
		      struct logical_volume *lv)
{
}
*/

int create_mirror_layers(struct alloc_handle *ah,
			 uint32_t first_area,
			 uint32_t num_mirrors,
			 struct logical_volume *lv,
			 struct segment_type *segtype,
			 uint32_t status,
			 uint32_t region_size,
			 struct logical_volume *log_lv)
{
	uint32_t m;
	struct logical_volume **img_lvs;
	char *img_name;
	size_t len;
	
	if (!(img_lvs = alloca(sizeof(*img_lvs) * num_mirrors))) {
		log_error("img_lvs allocation failed. "
			  "Remove new LV and retry.");
		return 0;
	}

	len = strlen(lv->name) + 32;
	if (!(img_name = alloca(len))) {
		log_error("img_name allocation failed. "
			  "Remove new LV and retry.");
		return 0;
	}

	if (lvm_snprintf(img_name, len, "%s_mimage_%%d", lv->name) < 0) {
		log_error("img_name allocation failed. "
			  "Remove new LV and retry.");
		return 0;
	}

	for (m = 0; m < num_mirrors; m++) {
		if (!(img_lvs[m] = lv_create_empty(lv->vg->fid, img_name,
					     NULL, LVM_READ | LVM_WRITE,
					     ALLOC_INHERIT, 0, lv->vg))) {\
			log_error("Aborting. Failed to create submirror LV. "
				  "Remove new LV and retry.");
			return 0;
		}

		if (!lv_add_segment(ah, m, 1, img_lvs[m],
				    get_segtype_from_string(lv->vg->cmd,
							    "striped"),
				    0, NULL, 0, 0, 0, NULL)) {
			log_error("Aborting. Failed to add submirror segment "
				  "to %s. Remove new LV and retry.",
				  img_lvs[m]->name);
			return 0;
		}
	}

	if (!lv_add_mirror_segment(ah, lv, img_lvs, num_mirrors, segtype,
				   0, region_size, log_lv)) {
		log_error("Aborting. Failed to add mirror segment. "
			  "Remove new LV and retry.");
		return 0;
	}

	return 1;
}

/* 
 * Replace any LV segments on given PV with temporary mirror.
 * Returns list of LVs changed.
 */
int insert_pvmove_mirrors(struct cmd_context *cmd,
			  struct logical_volume *lv_mirr,
			  struct list *source_pvl,
			  struct logical_volume *lv,
			  struct list *allocatable_pvs,
			  alloc_policy_t alloc,
			  struct list *lvs_changed)
{
	struct lv_segment *seg;
	struct lv_list *lvl;
	struct pv_list *pvl;
	struct physical_volume *pv;
	uint32_t pe;
	int lv_used = 0;
	uint32_t s, start_le, extent_count = 0u;
	struct segment_type *segtype;
	struct pe_range *per;
	uint32_t pe_start, pe_end, per_end, stripe_multiplier;

	/* Only 1 PV may feature in source_pvl */
	pvl = list_item(source_pvl->n, struct pv_list);

	if (!(segtype = get_segtype_from_string(lv->vg->cmd, "mirror"))) {
		stack;
		return 0;
	}

        if (activation() && segtype->ops->target_present &&
            !segtype->ops->target_present()) {
                log_error("%s: Required device-mapper target(s) not "
                          "detected in your kernel", segtype->name);
                return 0;
        }

	/* Split LV segments to match PE ranges */
	list_iterate_items(seg, &lv->segments) {
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) != AREA_PV ||
			    seg_dev(seg, s) != pvl->pv->dev)
				continue;

			/* Do these PEs need moving? */
			list_iterate_items(per, pvl->pe_ranges) {
				pe_start = seg_pe(seg, s);
				pe_end = pe_start + seg->area_len - 1;
				per_end = per->start + per->count - 1;

				/* No overlap? */
				if ((pe_end < per->start) ||
				    (pe_start > per_end))
					continue;

				if (seg_is_striped(seg))
					stripe_multiplier = seg->area_count;
				else
					stripe_multiplier = 1;

				if ((per->start != pe_start &&
				     per->start > pe_start) &&
				    !lv_split_segment(lv, seg->le +
						      (per->start - pe_start) *
						      stripe_multiplier)) {
					stack;
					return 0;
				}

				if ((per_end != pe_end &&
				     per_end < pe_end) &&
				    !lv_split_segment(lv, seg->le +
						      (per_end - pe_start + 1) *
						      stripe_multiplier)) {
					stack;
					return 0;
				}
			}
		}
	}

	/* Work through all segments on the supplied PV */
	list_iterate_items(seg, &lv->segments) {
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) != AREA_PV ||
			    seg_dev(seg, s) != pvl->pv->dev)
				continue;

			pe_start = seg_pe(seg, s);

			/* Do these PEs need moving? */
			list_iterate_items(per, pvl->pe_ranges) {
				per_end = per->start + per->count - 1;

				if ((pe_start < per->start) ||
				    (pe_start > per_end))
					continue;

				log_debug("Matched PE range %u-%u against "
					  "%s %u len %u", per->start, per_end,
					  dev_name(seg_dev(seg, s)),
					  seg_pe(seg, s),
					  seg->area_len);

				/* First time, add LV to list of LVs affected */
				if (!lv_used) {
					if (!(lvl = dm_pool_alloc(cmd->mem, sizeof(*lvl)))) {
						log_error("lv_list alloc failed");
						return 0;
					}
					lvl->lv = lv;
					list_add(lvs_changed, &lvl->list);
					lv_used = 1;
				}
	
				pv = seg_pv(seg, s);
				pe = seg_pe(seg, s);
				log_very_verbose("Moving %s:%u-%u of %s/%s",
						 dev_name(pvl->pv->dev),
						 pe, pe + seg->area_len - 1,
						 lv->vg->name, lv->name);

				start_le = lv_mirr->le_count;
				/* FIXME Clean this up */
				release_lv_segment_area(seg, s, seg->area_len);
				if (!lv_extend(lv_mirr, segtype, 1,
				       	seg->area_len, 0u, seg->area_len,
				       	pv, pe,
				       	PVMOVE, allocatable_pvs,
				       	alloc)) {
					log_error("Unable to allocate "
						  "temporary LV for pvmove.");
					return 0;
				}
				set_lv_segment_area_lv(seg, s, lv_mirr, start_le, 0);
	
				extent_count += seg->area_len;
	
				lv->status |= LOCKED;

				break;
			}
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
	struct lv_list *lvl;
	struct logical_volume *lv1;
	struct lv_segment *seg, *mir_seg;
	uint32_t s, c;

	/* Loop through all LVs except the temporary mirror */
	list_iterate_items(lvl, &vg->lvs) {
		lv1 = lvl->lv;
		if (lv1 == lv_mirr)
			continue;

		/* Find all segments that point at the temporary mirror */
		list_iterate_items(seg, &lv1->segments) {
			for (s = 0; s < seg->area_count; s++) {
				if (seg_type(seg, s) != AREA_LV ||
				    seg_lv(seg, s) != lv_mirr)
					continue;

				/* Find the mirror segment pointed at */
				if (!(mir_seg = find_seg_by_le(lv_mirr,
							       seg_le(seg, s)))) {
					/* FIXME Error message */
					log_error("No segment found with LE");
					return 0;
				}

				/* Check the segment params are compatible */
				/* FIXME Improve error mesg & remove restrcn */
				if (!seg_is_mirrored(mir_seg) ||
				    !(mir_seg->status & PVMOVE) ||
				    mir_seg->le != seg_le(seg, s) ||
				    mir_seg->area_count != 2 ||
				    mir_seg->area_len != seg->area_len) {
					log_error("Incompatible segments");
					return 0;
				}

				/* Replace original segment with newly-mirrored
				 * area (or original if reverting)
				 */
				if (mir_seg->extents_copied == 
				        mir_seg->area_len)
					c = 1;
				else
					c = 0;

				if (!move_lv_segment_area(seg, s, mir_seg, c)) {
					stack;
					return 0;
				}

				release_lv_segment_area(mir_seg, !c, mir_seg->area_len);

				/* Replace mirror with error segment */
				if (!
				    (mir_seg->segtype =
				     get_segtype_from_string(vg->cmd,
							     "error"))) {
					log_error("Missing error segtype");
					return 0;
				}
				mir_seg->area_count = 0;

				/* FIXME Assumes only one pvmove at a time! */
				lv1->status &= ~LOCKED;
			}
		}
		if (!lv_merge_segments(lv1))
			stack;

	}

	if (!lv_empty(lv_mirr)) {
		stack;
		return 0;
	}

	return 1;
}

const char *get_pvmove_pvname_from_lv_mirr(struct logical_volume *lv_mirr)
{
	struct lv_segment *seg;

	list_iterate_items(seg, &lv_mirr->segments) {
		if (!seg_is_mirrored(seg))
			continue;
		if (seg_type(seg, 0) != AREA_PV)
			continue;
		return dev_name(seg_dev(seg, 0));
	}

	return NULL;
}

const char *get_pvmove_pvname_from_lv(struct logical_volume *lv)
{
	struct lv_segment *seg;
	uint32_t s;

	list_iterate_items(seg, &lv->segments) {
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) != AREA_LV)
				continue;
			return get_pvmove_pvname_from_lv_mirr(seg_lv(seg, s));
		}
	}

	return NULL;
}

struct logical_volume *find_pvmove_lv(struct volume_group *vg,
				      struct device *dev,
				      uint32_t lv_type)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct lv_segment *seg;

	/* Loop through all LVs */
	list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!(lv->status & lv_type))
			continue;

		/* Check segment origins point to pvname */
		list_iterate_items(seg, &lv->segments) {
			if (seg_type(seg, 0) != AREA_PV)
				continue;
			if (seg_dev(seg, 0) != dev)
				continue;
			return lv;
		}
	}

	return NULL;
}

struct logical_volume *find_pvmove_lv_from_pvname(struct cmd_context *cmd,
					 	  struct volume_group *vg,
				      		  const char *name,
				      		  uint32_t lv_type)
{
	struct physical_volume *pv;

	if (!(pv = find_pv_by_name(cmd, name))) {
		stack;
		return NULL;
	}

	return find_pvmove_lv(vg, pv->dev, lv_type);
}

struct list *lvs_using_lv(struct cmd_context *cmd, struct volume_group *vg,
			  struct logical_volume *lv)
{
	struct list *lvs;
	struct logical_volume *lv1;
	struct lv_list *lvl, *lvl1;
	struct lv_segment *seg;
	uint32_t s;

	if (!(lvs = dm_pool_alloc(cmd->mem, sizeof(*lvs)))) {
		log_error("lvs list alloc failed");
		return NULL;
	}

	list_init(lvs);

	/* Loop through all LVs except the one supplied */
	list_iterate_items(lvl1, &vg->lvs) {
		lv1 = lvl1->lv;
		if (lv1 == lv)
			continue;

		/* Find whether any segment points at the supplied LV */
		list_iterate_items(seg, &lv1->segments) {
			for (s = 0; s < seg->area_count; s++) {
				if (seg_type(seg, s) != AREA_LV ||
				    seg_lv(seg, s) != lv)
					continue;
				if (!(lvl = dm_pool_alloc(cmd->mem, sizeof(*lvl)))) {
					log_error("lv_list alloc failed");
					return NULL;
				}
				lvl->lv = lv1;
				list_add(lvs, &lvl->list);
				goto next_lv;
			}
		}
	      next_lv:
		;
	}

	return lvs;
}

float copy_percent(struct logical_volume *lv_mirr)
{
	uint32_t numerator = 0u, denominator = 0u;
	struct lv_segment *seg;

	list_iterate_items(seg, &lv_mirr->segments) {
		denominator += seg->area_len;

		if (seg_is_mirrored(seg))
			numerator += seg->extents_copied;
		else
			numerator += seg->area_len;
	}

	return denominator ? (float) numerator *100 / denominator : 100.0;
}
