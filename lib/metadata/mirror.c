/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
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
#include "str_list.h"
#include "locking.h"	/* FIXME Should not be used in this file */

#include "defaults.h" /* FIXME: should this be defaults.h? */

/* These are the flags that represent the mirror failure restoration policies */
#define MIRROR_REMOVE            0
#define MIRROR_ALLOCATE          1
#define MIRROR_ALLOCATE_ANYWHERE 2

struct lv_segment *find_mirror_seg(struct lv_segment *seg)
{
	return seg->mirror_seg;
}

/*
 * Reduce the region size if necessary to ensure
 * the volume size is a multiple of the region size.
 */
uint32_t adjusted_mirror_region_size(uint32_t extent_size, uint32_t extents,
				     uint32_t region_size)
{
	uint64_t region_max;

	region_max = (1 << (ffs((int)extents) - 1)) * (uint64_t) extent_size;

	if (region_max < UINT32_MAX && region_size > region_max) {
		region_size = (uint32_t) region_max;
		log_print("Using reduced mirror region size of %" PRIu32
			  " sectors", region_size);
	}

	return region_size;
}

static void _move_lv_segments(struct logical_volume *lv_to, struct logical_volume *lv_from)
{
	struct lv_segment *seg;

	lv_to->segments = lv_from->segments;
	lv_to->segments.n->p = &lv_to->segments;
	lv_to->segments.p->n = &lv_to->segments;

	list_iterate_items(seg, &lv_to->segments)
		seg->lv = lv_to;

/* FIXME set or reset seg->mirror_seg (according to status)? */

	list_init(&lv_from->segments);

	lv_to->le_count = lv_from->le_count;
	lv_to->size = lv_from->size;

	lv_from->le_count = 0;
	lv_from->size = 0;
}


/*
 * Delete independent/orphan LV, it must acquire lock.
 */
static int _delete_lv(struct lv_segment *mirrored_seg, struct logical_volume *lv)
{
	struct cmd_context *cmd = mirrored_seg->lv->vg->cmd;
	struct str_list *sl;

	/* Inherit tags - maybe needed for activation */
	if (!str_list_match_list(&mirrored_seg->lv->tags, &lv->tags)) {
		list_iterate_items(sl, &mirrored_seg->lv->tags)
			if (!str_list_add(cmd->mem, &lv->tags, sl->str)) {
				log_error("Aborting. Unable to tag.");
				return 0;
			}

		if (!vg_write(mirrored_seg->lv->vg) ||
		    !vg_commit(mirrored_seg->lv->vg)) {
			log_error("Intermediate VG commit for orphan volume failed.");
			return 0;
		}
	}

	if (!activate_lv(cmd, lv))
		return_0;

	if (!deactivate_lv(cmd, lv))
		return_0;

	if (!lv_remove(lv))
		return_0;

	return 1;
}

/*
 * Reduce mirrored_seg to num_mirrors images.
 */
int remove_mirror_images(struct lv_segment *mirrored_seg, uint32_t num_mirrors,
			 struct list *removable_pvs, int remove_log)
{
	uint32_t m;
	uint32_t extents;
	uint32_t s, s1;
	struct logical_volume *sub_lv;
	struct logical_volume *log_lv = NULL;
	struct logical_volume *lv1 = NULL;
	struct physical_volume *pv;
	struct lv_segment *seg;
	struct lv_segment_area area;
	int all_pvs_removable, pv_found;
	struct pv_list *pvl;
	uint32_t old_area_count = mirrored_seg->area_count;
	uint32_t new_area_count = mirrored_seg->area_count;
	struct segment_type *segtype;

	log_very_verbose("Reducing mirror set from %" PRIu32 " to %"
			 PRIu32 " image(s)%s.",
			 old_area_count, num_mirrors,
			 remove_log ? " and no log volume" : "");

	/* Move removable_pvs to end of array */
	if (removable_pvs) {
		for (s = 0; s < mirrored_seg->area_count; s++) {
			all_pvs_removable = 1;
			sub_lv = seg_lv(mirrored_seg, s);
			list_iterate_items(seg, &sub_lv->segments) {
				for (s1 = 0; s1 < seg->area_count; s1++) {
					if (seg_type(seg, s1) != AREA_PV)
						/* FIXME Recurse for AREA_LV */
						continue;

					pv = seg_pv(seg, s1);

					pv_found = 0;
					list_iterate_items(pvl, removable_pvs) {
						if (pv->dev->dev == pvl->pv->dev->dev) {
							pv_found = 1;
							break;
						}
					}
					if (!pv_found) {
						all_pvs_removable = 0;
						break;
					}
				}
				if (!all_pvs_removable)
					break;
			}
			if (all_pvs_removable) {
				/* Swap segment to end */
				new_area_count--;
				area = mirrored_seg->areas[new_area_count];
				mirrored_seg->areas[new_area_count] = mirrored_seg->areas[s];
				mirrored_seg->areas[s] = area;
			}
			/* Found enough matches? */
			if (new_area_count == num_mirrors)
				break;
		}
		if (new_area_count == mirrored_seg->area_count) {
			log_error("No mirror images found using specified PVs.");
			return 0;
		}
	}

	for (m = num_mirrors; m < mirrored_seg->area_count; m++) {
		seg_lv(mirrored_seg, m)->status &= ~MIRROR_IMAGE;
		seg_lv(mirrored_seg, m)->status |= VISIBLE_LV;
	}

	mirrored_seg->area_count = num_mirrors;

	/* If no more mirrors, remove mirror layer */
	if (num_mirrors == 1) {
		lv1 = seg_lv(mirrored_seg, 0);
		extents = lv1->le_count;
		_move_lv_segments(mirrored_seg->lv, lv1);
		mirrored_seg->lv->status &= ~MIRRORED;
		remove_log = 1;
		/* Replace mirror with error segment */
		segtype = get_segtype_from_string(mirrored_seg->lv->vg->cmd, "error");
		if (!lv_add_virtual_segment(lv1, 0, extents, segtype))
			return_0;
	}

	if (remove_log && mirrored_seg->log_lv) {
		log_lv = mirrored_seg->log_lv;
		mirrored_seg->log_lv = NULL;
		log_lv->status &= ~MIRROR_LOG;
		log_lv->status |= VISIBLE_LV;
	}

	/*
	 * To successfully remove these unwanted LVs we need to
	 * remove the LVs from the mirror set, commit that metadata
	 * then deactivate and remove them fully.
	 */

	if (!vg_write(mirrored_seg->lv->vg)) {
		log_error("intermediate VG write failed.");
		return 0;
	}

	if (!suspend_lv(mirrored_seg->lv->vg->cmd, mirrored_seg->lv)) {
		log_error("Failed to lock %s", mirrored_seg->lv->name);
		vg_revert(mirrored_seg->lv->vg);
		return 0;
	}

	if (!vg_commit(mirrored_seg->lv->vg)) {
		resume_lv(mirrored_seg->lv->vg->cmd, mirrored_seg->lv);
		return 0;
	}

	log_very_verbose("Updating \"%s\" in kernel", mirrored_seg->lv->name);

	if (!resume_lv(mirrored_seg->lv->vg->cmd, mirrored_seg->lv)) {
		log_error("Problem reactivating %s", mirrored_seg->lv->name);
		return 0;
	}

	/* Delete the 'orphan' LVs */
	for (m = num_mirrors; m < old_area_count; m++)
		if (!_delete_lv(mirrored_seg, seg_lv(mirrored_seg, m)))
			return 0;

	if (lv1 && !_delete_lv(mirrored_seg, lv1))
		return 0;

	if (log_lv && !_delete_lv(mirrored_seg, log_lv))
		return 0;

	return 1;
}

static int get_mirror_fault_policy(struct cmd_context *cmd, int log_policy)
{
	const char *policy;

	if (log_policy)
		policy = find_config_str(NULL, "activation/mirror_log_fault_policy",
					 DEFAULT_MIRROR_LOG_FAULT_POLICY);
	else
		policy = find_config_str(NULL, "activation/mirror_device_fault_policy",
					 DEFAULT_MIRROR_DEV_FAULT_POLICY);

	if (!strcmp(policy, "remove"))
		return MIRROR_REMOVE;
	else if (!strcmp(policy, "allocate"))
		return MIRROR_ALLOCATE;
	else if (!strcmp(policy, "allocate_anywhere"))
		return MIRROR_ALLOCATE_ANYWHERE;

	if (log_policy)
		log_error("Bad activation/mirror_log_fault_policy");
	else
		log_error("Bad activation/mirror_device_fault_policy");

	return MIRROR_REMOVE;
}

static int get_mirror_log_fault_policy(struct cmd_context *cmd)
{
	return get_mirror_fault_policy(cmd, 1);
}

static int get_mirror_device_fault_policy(struct cmd_context *cmd)
{
	return get_mirror_fault_policy(cmd, 0);
}

/*
 * replace_mirror_images
 * @mirrored_seg: segment (which may be linear now) to restore
 * @num_mirrors: number of copies we should end up with
 * @replace_log: replace log if not present
 * @in_sync: was the original mirror in-sync?
 *
 * in_sync will be set to 0 if new mirror devices are being added
 * In other words, it is only useful if the log (and only the log)
 * is being restored.
 *
 * Returns: 0 on failure, 1 on reconfig, -1 if no reconfig done
 */
static int replace_mirror_images(struct lv_segment *mirrored_seg,
				 uint32_t num_mirrors,
				 int log_policy, int in_sync)
{
	int r = -1;
	struct logical_volume *lv = mirrored_seg->lv;

	/* FIXME: Use lvconvert rather than duplicating its code */

	if (mirrored_seg->area_count < num_mirrors) {
		log_error("WARNING: Failed to replace mirror device in %s/%s",
			  mirrored_seg->lv->vg->name, mirrored_seg->lv->name);

		if ((mirrored_seg->area_count > 1) && !mirrored_seg->log_lv)
			log_error("WARNING: Use 'lvconvert -m %d %s/%s --corelog' to replace failed devices",
				  num_mirrors - 1, lv->vg->name, lv->name);
		else
			log_error("WARNING: Use 'lvconvert -m %d %s/%s' to replace failed devices",
				  num_mirrors - 1, lv->vg->name, lv->name);
		r = 0;

		/* REMEMBER/FIXME: set in_sync to 0 if a new mirror device was added */
		in_sync = 0;
	}

	/*
	 * FIXME: right now, we ignore the allocation policy specified to
	 * allocate the new log.
	 */
	if ((mirrored_seg->area_count > 1) && !mirrored_seg->log_lv &&
	    (log_policy != MIRROR_REMOVE)) {
		log_error("WARNING: Failed to replace mirror log device in %s/%s",
			  lv->vg->name, lv->name);

		log_error("WARNING: Use 'lvconvert -m %d %s/%s' to replace failed devices",
			  mirrored_seg->area_count - 1 , lv->vg->name, lv->name);
		r = 0;
	}

	return r;
}

int reconfigure_mirror_images(struct lv_segment *mirrored_seg, uint32_t num_mirrors,
			      struct list *removable_pvs, int remove_log)
{
	int r;
	int insync = 0;
	int log_policy, dev_policy;
	uint32_t old_num_mirrors = mirrored_seg->area_count;
	int had_log = (mirrored_seg->log_lv) ? 1 : 0;
	float sync_percent = 0;

	/* was the mirror in-sync before problems? */
	if (!lv_mirror_percent(mirrored_seg->lv->vg->cmd,
			       mirrored_seg->lv, 0, &sync_percent, NULL))
		log_error("WARNING: Unable to determine mirror sync status of %s/%s.",
			  mirrored_seg->lv->vg->name, mirrored_seg->lv->name);
	else if (sync_percent >= 100.0)
		insync = 1;

	/*
	 * While we are only removing devices, we can have sync set.
	 * Setting this is only useful if we are moving to core log
	 * otherwise the disk log will contain the sync information
	 */
	init_mirror_in_sync(insync);

	r = remove_mirror_images(mirrored_seg, num_mirrors,
				 removable_pvs, remove_log);
	if (!r)
		/* Unable to remove bad devices */
		return 0;

	log_warn("WARNING: Bad device removed from mirror volume, %s/%s",
		  mirrored_seg->lv->vg->name, mirrored_seg->lv->name);

	log_policy = get_mirror_log_fault_policy(mirrored_seg->lv->vg->cmd);
	dev_policy = get_mirror_device_fault_policy(mirrored_seg->lv->vg->cmd);

	r = replace_mirror_images(mirrored_seg,
				  (dev_policy != MIRROR_REMOVE) ?
				  old_num_mirrors : num_mirrors,
				  log_policy, insync);

	if (!r)
		/* Failed to replace device(s) */
		log_error("WARNING: Unable to find substitute device for mirror volume, %s/%s",
			  mirrored_seg->lv->vg->name, mirrored_seg->lv->name);
	else if (r > 0)
		/* Success in replacing device(s) */
		log_warn("WARNING: Mirror volume, %s/%s restored - substitute for failed device found.",
			  mirrored_seg->lv->vg->name, mirrored_seg->lv->name);
	else
		/* Bad device removed, but not replaced because of policy */
		if (mirrored_seg->area_count == 1) {
			log_warn("WARNING: Mirror volume, %s/%s converted to linear due to device failure.",
				  mirrored_seg->lv->vg->name, mirrored_seg->lv->name);
		} else if (had_log && !mirrored_seg->log_lv) {
			log_warn("WARNING: Mirror volume, %s/%s disk log removed due to device failure.",
				  mirrored_seg->lv->vg->name, mirrored_seg->lv->name);
		}
	/*
	 * If we made it here, we at least removed the bad device.
	 * Consider this success.
	 */
	return 1;
}

static int _create_layers_for_mirror(struct alloc_handle *ah,
				     uint32_t first_area,
				     uint32_t num_mirrors,
				     struct logical_volume *lv,
				     const struct segment_type *segtype,
				     struct logical_volume **img_lvs)
{
	uint32_t m;
	char *img_name;
	size_t len;
	
	len = strlen(lv->name) + 32;
	if (!(img_name = alloca(len))) {
		log_error("img_name allocation failed. "
			  "Remove new LV and retry.");
		return 0;
	}

	if (dm_snprintf(img_name, len, "%s_mimage_%%d", lv->name) < 0) {
		log_error("img_name allocation failed. "
			  "Remove new LV and retry.");
		return 0;
	}

	for (m = 0; m < num_mirrors; m++) {
		if (!(img_lvs[m] = lv_create_empty(lv->vg->fid, img_name,
					     NULL, LVM_READ | LVM_WRITE,
					     ALLOC_INHERIT, 0, lv->vg))) {
			log_error("Aborting. Failed to create mirror image LV. "
				  "Remove new LV and retry.");
			return 0;
		}

		if (m < first_area)
			continue;

		if (!lv_add_segment(ah, m - first_area, 1, img_lvs[m],
				    get_segtype_from_string(lv->vg->cmd,
							    "striped"),
				    0, NULL, 0, 0, 0, NULL)) {
			log_error("Aborting. Failed to add mirror image segment "
				  "to %s. Remove new LV and retry.",
				  img_lvs[m]->name);
			return 0;
		}
	}

	return 1;
}

int create_mirror_layers(struct alloc_handle *ah,
			 uint32_t first_area,
			 uint32_t num_mirrors,
			 struct logical_volume *lv,
			 const struct segment_type *segtype,
			 uint32_t status,
			 uint32_t region_size,
			 struct logical_volume *log_lv)
{
	struct logical_volume **img_lvs;
	
	if (!(img_lvs = alloca(sizeof(*img_lvs) * num_mirrors))) {
		log_error("img_lvs allocation failed. "
			  "Remove new LV and retry.");
		return 0;
	}

	if (!_create_layers_for_mirror(ah, first_area, num_mirrors, lv,
				       segtype, img_lvs)) {
		stack;
		return 0;
	}

	/* Already got the parent mirror segment? */
	if (lv->status & MIRRORED)
		return lv_add_more_mirrored_areas(lv, img_lvs, num_mirrors,
						  MIRROR_IMAGE);

	/* Already got a non-mirrored area to be converted? */
	if (first_area)
		_move_lv_segments(img_lvs[0], lv);

	if (!lv_add_mirror_segment(ah, lv, img_lvs, num_mirrors, segtype,
				   0, region_size, log_lv)) {
		log_error("Aborting. Failed to add mirror segment. "
			  "Remove new LV and retry.");
		return 0;
	}

	lv->status |= MIRRORED;

	return 1;
}

int add_mirror_layers(struct alloc_handle *ah,
		      uint32_t num_mirrors,
		      uint32_t existing_mirrors,
		      struct logical_volume *lv,
		      const struct segment_type *segtype)
{
	struct logical_volume **img_lvs;

	if (!(img_lvs = alloca(sizeof(*img_lvs) * num_mirrors))) {
		log_error("img_lvs allocation failed. "
			  "Remove new LV and retry.");
		return 0;
	}

	if (!_create_layers_for_mirror(ah, 0, num_mirrors,
				       lv, segtype,
				       img_lvs)) {
		stack;
		return 0;
	}

	return lv_add_more_mirrored_areas(lv, img_lvs, num_mirrors, 0);
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
	const struct segment_type *segtype;
	struct pe_range *per;
	uint32_t pe_start, pe_end, per_end, stripe_multiplier;

	/* Only 1 PV may feature in source_pvl */
	pvl = list_item(source_pvl->n, struct pv_list);

	if (!(segtype = get_segtype_from_string(lv->vg->cmd, "mirror"))) {
		stack;
		return 0;
	}

        if (activation() && segtype->ops->target_present &&
            !segtype->ops->target_present(NULL)) {
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

				release_lv_segment_area(mir_seg, c ? 0 : 1U, mir_seg->area_len);

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

/*
 * Fixup mirror pointers after single-pass segment import
 */
int fixup_imported_mirrors(struct volume_group *vg)
{
	struct lv_list *lvl;
	struct lv_segment *seg;
	uint32_t s;

	list_iterate_items(lvl, &vg->lvs) {
		list_iterate_items(seg, &lvl->lv->segments) {
			if (seg->segtype !=
			    get_segtype_from_string(vg->cmd, "mirror"))
				continue;

			if (seg->log_lv)
				first_seg(seg->log_lv)->mirror_seg = seg;
			for (s = 0; s < seg->area_count; s++)
				if (seg_type(seg, s) == AREA_LV)
					first_seg(seg_lv(seg, s))->mirror_seg
					    = seg;
		}
	}

	return 1;
}

