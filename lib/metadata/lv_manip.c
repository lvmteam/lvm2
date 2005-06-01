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

#include "lib.h"
#include "metadata.h"
#include "locking.h"
#include "pv_map.h"
#include "lvm-string.h"
#include "toolcontext.h"
#include "lv_alloc.h"
#include "pv_alloc.h"
#include "display.h"
#include "segtype.h"

/*
 * Find first unused LV number.
 */
uint32_t find_free_lvnum(struct logical_volume *lv)
{
	int lvnum_used[MAX_RESTRICTED_LVS + 1];
	uint32_t i = 0;
	struct lv_list *lvl;
	int lvnum;

	memset(&lvnum_used, 0, sizeof(lvnum_used));

	list_iterate_items(lvl, &lv->vg->lvs) {
		lvnum = lvnum_from_lvid(&lvl->lv->lvid);
		if (lvnum <= MAX_RESTRICTED_LVS)
			lvnum_used[lvnum] = 1;
	}

	while (lvnum_used[i])
		i++;

	/* FIXME What if none are free? */

	return i;
}

/*
 * All lv_segments get created here.
 */
struct lv_segment *alloc_lv_segment(struct pool *mem,
				    struct segment_type *segtype,
				    struct logical_volume *lv,
				    uint32_t le, uint32_t len,
				    uint32_t status,
				    uint32_t stripe_size,
				    struct logical_volume *log_lv,
				    uint32_t area_count,
				    uint32_t area_len,
				    uint32_t chunk_size,
				    uint32_t region_size,
				    uint32_t extents_copied)
{
	struct lv_segment *seg;
	uint32_t sz = sizeof(*seg) + (area_count * sizeof(seg->area[0]));

	if (!(seg = pool_zalloc(mem, sz))) {
		stack;
		return NULL;
	}

	seg->segtype = segtype;
	seg->lv = lv;
	seg->le = le;
	seg->len = len;
	seg->status = status;
	seg->stripe_size = stripe_size;
	seg->area_count = area_count;
	seg->area_len = area_len;
	seg->chunk_size = chunk_size;
	seg->region_size = region_size;
	seg->extents_copied = extents_copied;
	seg->log_lv = log_lv;
	list_init(&seg->tags);

	if (log_lv)
		log_lv->status |= MIRROR_LOG;

	return seg;
}

struct lv_segment *alloc_snapshot_seg(struct logical_volume *lv,
				      uint32_t status, uint32_t old_le_count)
{
	struct lv_segment *seg;
	struct segment_type *segtype;

	segtype = get_segtype_from_string(lv->vg->cmd, "snapshot");
	if (!segtype) {
		log_error("Failed to find snapshot segtype");
		return NULL;
	}

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv, old_le_count,
				     lv->le_count - old_le_count, status, 0,
				     NULL, 0, lv->le_count - old_le_count,
				     0, 0, 0))) {
		log_error("Couldn't allocate new snapshot segment.");
		return NULL;
	}

	list_add(&lv->segments, &seg->list);
	lv->status |= VIRTUAL;

	return seg;
}

/*
 * Link part of a PV to an LV segment.
 */
int set_lv_segment_area_pv(struct lv_segment *seg, uint32_t area_num,
			   struct physical_volume *pv, uint32_t pe)
{
	seg->area[area_num].type = AREA_PV;
	pv->pe_alloc_count += seg->area_len;

	if (!(seg_pvseg(seg, area_num) =
	      assign_peg_to_lvseg(pv, pe, seg->area_len, seg, area_num))) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Link one LV segment to another.  Assumes sizes already match.
 */
void set_lv_segment_area_lv(struct lv_segment *seg, uint32_t area_num,
			    struct logical_volume *lv, uint32_t le)
{
	seg->area[area_num].type = AREA_LV;
	seg_lv(seg, area_num) = lv;
	seg_le(seg, area_num) = le;
}

/*
 * Reduce the size of an lv_segment.  New size can be zero.
 */
static int _lv_segment_reduce(struct lv_segment *seg, uint32_t reduction)
{
	uint32_t area_reduction, s;

	/* Caller must ensure exact divisibility */
	if (seg_is_striped(seg)) {
		if (reduction % seg->area_count) {
			log_error("Segment extent reduction %" PRIu32
				  "not divisible by #stripes %" PRIu32,
				  reduction, seg->area_count);
			return 0;
		}
		area_reduction = (reduction / seg->area_count);
	} else
		area_reduction = reduction;

	seg->len -= reduction;
	seg->area_len -= area_reduction;
	seg->lv->vg->free_count += area_reduction * seg->area_count;

	for (s = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) != AREA_PV)
			continue;
		release_pv_segment(seg_pvseg(seg, s), area_reduction);
	}

	return 1;
}

/*
 * Entry point for all LV reductions in size.
 */
int lv_reduce(struct logical_volume *lv, uint32_t extents)
{
	struct lv_segment *seg;
	uint32_t count = extents;
	uint32_t reduction;

	list_iterate_back_items(seg, &lv->segments) {
		if (!count)
			break;

		if (seg->len <= count) {
			/* remove this segment completely */
			/* FIXME Check this is safe */
			if (seg->log_lv && !lv_remove(seg->log_lv)) {
				stack;
				return 0;
			}
			list_del(&seg->list);
			reduction = seg->len;
		} else
			reduction = count;

		if (!_lv_segment_reduce(seg, reduction)) {
			stack;
			return 0;
		}
		count -= reduction;
	}

	lv->le_count -= extents;
	lv->size = (uint64_t) lv->le_count * lv->vg->extent_size;

	if (lv->le_count && lv->vg->fid->fmt->ops->lv_setup &&
	    !lv->vg->fid->fmt->ops->lv_setup(lv->vg->fid, lv)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Completely remove an LV.
 */
int lv_remove(struct logical_volume *lv)
{
	struct lv_list *lvl;

	if (!lv_reduce(lv, lv->le_count)) {
		stack;
		return 0;
	}

	/* find the lv list */
	if (!(lvl = find_lv_in_vg(lv->vg, lv->name))) {
		stack;
		return 0;
	}

	list_del(&lvl->list);

	lv->vg->lv_count--;

	return 1;
}

/*
 * A set of contiguous physical extents allocated
 */
struct alloced_area {
	struct list list;

	struct physical_volume *pv;
	uint32_t pe;
	uint32_t len;
};

/*
 * Details of an allocation attempt
 */
struct alloc_handle {
	struct pool *mem;

	alloc_policy_t alloc;		/* Overall policy */
	uint32_t area_count;		/* Number of parallel areas */
	uint32_t area_multiple;		/* seg->len = area_len * area_multiple */
	uint32_t log_count;		/* Number of parallel 1-extent logs */

	struct alloced_area log_area;	/* Extent used for log */
	struct list alloced_areas[0];	/* Lists of areas in each stripe */
};

/*
 * Preparation for a specific allocation attempt
 */
static struct alloc_handle *_alloc_init(struct pool *mem,
					struct segment_type *segtype,
					alloc_policy_t alloc,
					uint32_t mirrors,
					uint32_t stripes,
					uint32_t log_count,
					struct physical_volume *mirrored_pv)
{
	struct alloc_handle *ah;
	uint32_t s, area_count;

	if (stripes > 1 && mirrors > 1) {
		log_error("striped mirrors are not supported yet");
		return NULL;
	}

	if ((stripes > 1 || mirrors > 1) && mirrored_pv) {
		log_error("Can't mix striping or mirroring with "
			  "creation of a mirrored PV yet");
		return NULL;
	}

	if (log_count && (stripes > 1 || mirrored_pv)) {
		log_error("Can't mix striping or pvmove with "
			  "a mirror log yet.");
		return NULL;
	}

	if (segtype_is_virtual(segtype))
		area_count = 0;
	else if (mirrors > 1)
		area_count = mirrors;
	else if (mirrored_pv)
		area_count = 1;
	else
		area_count = stripes;

	if (!(ah = pool_zalloc(mem, sizeof(*ah) + sizeof(ah->alloced_areas[0]) * area_count))) {
		log_error("allocation handle allocation failed");
		return NULL;
	}

	if (segtype_is_virtual(segtype))
		return ah;

	if (!(ah->mem = pool_create("allocation", 1024))) {
		log_error("allocation pool creation failed");
		return NULL;
	}

	ah->area_count = area_count;
	ah->log_count = log_count;
	ah->alloc = alloc;
	ah->area_multiple = segtype_is_striped(segtype) ? ah->area_count : 1;

	list_init(&ah->alloced_areas[0]);

	for (s = 0; s < ah->area_count; s++)
		list_init(&ah->alloced_areas[s]);

	return ah;
}

void alloc_destroy(struct alloc_handle *ah)
{
	if (ah->mem)
		pool_destroy(ah->mem);
}

static int _setup_alloced_segment(struct logical_volume *lv, uint32_t status,
				  uint32_t area_count,
				  uint32_t stripe_size,
				  struct segment_type *segtype,
				  struct alloced_area *aa,
				  struct physical_volume *mirrored_pv,
				  uint32_t mirrored_pe,
				  uint32_t region_size,
				  struct logical_volume *log_lv)
{
	uint32_t s, extents, area_multiple, extra_areas = 0;
	struct lv_segment *seg;

	if (mirrored_pv)
		extra_areas = 1;

	area_multiple = segtype_is_striped(segtype) ? area_count : 1;

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv,
				     lv->le_count,
				     aa[0].len * area_multiple,
				     status, stripe_size, log_lv,
				     area_count + extra_areas,
				     aa[0].len, 0u, region_size, 0u))) {
		log_error("Couldn't allocate new LV segment.");
		return 0;
	}

	if (extra_areas) {
		if (!set_lv_segment_area_pv(seg, 0, mirrored_pv, mirrored_pe)) {
			stack;
			return 0;
		}
	}

	for (s = 0; s < area_count; s++) {
		if (!set_lv_segment_area_pv(seg, s + extra_areas, aa[s].pv,
					    aa[s].pe)) {
			stack;
			return 0;
		}
	}

	list_add(&lv->segments, &seg->list);

	extents = aa[0].len * area_multiple;
	lv->le_count += extents;
	lv->size += (uint64_t) extents *lv->vg->extent_size;

	lv->vg->free_count -= aa[0].len * area_count;

	if (segtype_is_mirrored(segtype))
		lv->status |= MIRRORED;

	return 1;
}

static int _setup_alloced_segments(struct logical_volume *lv,
				   struct list *alloced_areas,
				   uint32_t area_count,
				   uint32_t status,
				   uint32_t stripe_size,
				   struct segment_type *segtype,
				   struct physical_volume *mirrored_pv,
				   uint32_t mirrored_pe,
				   uint32_t region_size,
				   struct logical_volume *log_lv)
{
	struct alloced_area *aa;

	list_iterate_items(aa, &alloced_areas[0]) {
		if (!_setup_alloced_segment(lv, status, area_count,
					    stripe_size, segtype, aa,
					    mirrored_pv, mirrored_pe,
					    region_size, log_lv)) {
			stack;
			return 0;
		}
	}

	return 1;
}

/*
 * This function takes a list of pv_areas and adds them to allocated_areas.
 * If the complete area is not needed then it gets split.
 * The part used is removed from the pv_map so it can't be allocated twice.
 */
static int _alloc_parallel_area(struct alloc_handle *ah, uint32_t needed,
				struct pv_area **areas,
				uint32_t *ix, struct pv_area *log_area)
{
	uint32_t area_len, smallest, remaining;
	uint32_t s;
	struct alloced_area *aa;

	remaining = needed - *ix;
	area_len = remaining / ah->area_multiple;

	smallest = areas[ah->area_count - 1]->count;

	if (area_len > smallest)
		area_len = smallest;

	if (!(aa = pool_alloc(ah->mem, sizeof(*aa) *
			      (ah->area_count + (log_area ? 1 : 0))))) {
		log_error("alloced_area allocation failed");
		return 0;
	}

	for (s = 0; s < ah->area_count; s++) {
		aa[s].pv = areas[s]->map->pv;
		aa[s].pe = areas[s]->start;
		aa[s].len = area_len;
		list_add(&ah->alloced_areas[s], &aa[s].list);
	}

	for (s = 0; s < ah->area_count; s++)
		consume_pv_area(areas[s], area_len);

	if (log_area) {
		ah->log_area.pv = log_area->map->pv;
		ah->log_area.pe = log_area->start;
		ah->log_area.len = 1;	/* FIXME Calculate & check this */
		consume_pv_area(log_area, ah->log_area.len);
	}

	*ix += area_len * ah->area_multiple;

	return 1;
}

static int _comp_area(const void *l, const void *r)
{
	const struct pv_area *lhs = *((const struct pv_area **) l);
	const struct pv_area *rhs = *((const struct pv_area **) r);

	if (lhs->count < rhs->count)
		return 1;

	else if (lhs->count > rhs->count)
		return -1;

	return 0;
}

/*
 * Is pva contiguous to any existing areas or on the same PV?
 */
static int _check_contiguous(struct lv_segment *prev_lvseg,
			     struct physical_volume *pv, struct pv_area *pva,
			     struct pv_area **areas)
{
	struct pv_segment *prev_pvseg;
	uint32_t s;

	for (s = 0; s < prev_lvseg->area_count; s++) {
		if (seg_type(prev_lvseg, s) != AREA_PV)
			continue;	/* FIXME Broken */

		if (!(prev_pvseg = seg_pvseg(prev_lvseg, s)))
			continue; /* FIXME Broken */

		if ((prev_pvseg->pv != pv))
			continue;

		if (prev_pvseg->pe + prev_pvseg->len == pva->start) {
			areas[s] = pva;
			return 1;
		}
	}

	return 0;
}

/*
 * Choose sets of parallel areas to use, respecting any constraints.
 */
static int _find_parallel_space(struct alloc_handle *ah, alloc_policy_t alloc,
				struct list *pvms, struct pv_area **areas,
				uint32_t areas_size, unsigned can_split,
				struct lv_segment *prev_lvseg,
				uint32_t *allocated, uint32_t needed)
{
	struct pv_map *pvm;
	struct pv_area *pva;
	unsigned already_found_one = 0;
	unsigned contiguous = 0, contiguous_count = 0;
	unsigned ix;
	unsigned ix_offset = 0;	/* Offset for non-contiguous allocations */

	/* FIXME Do calculations on free extent counts before selecting space */
	/* FIXME Select log PV appropriately if there isn't one yet */

	if ((alloc == ALLOC_CONTIGUOUS)) {
		contiguous = 1;
		if (prev_lvseg)
			ix_offset = prev_lvseg->area_count;
		else
			ix_offset = ah->area_count;
	}

	/* FIXME This algorithm needs a lot of cleaning up! */
	/* FIXME anywhere doesn't find all space yet */
	/* ix_offset holds the number of allocations that must be contiguous */
	/* ix holds the number of areas found on other PVs */
	do {
		ix = 0;

		/*
		 * Put the smallest area of each PV that is at least the
		 * size we need into areas array.  If there isn't one
		 * that fits completely and we're allowed more than one
		 * LV segment, then take the largest remaining instead.
		 */
		list_iterate_items(pvm, pvms) {
			if (list_empty(&pvm->areas))
				continue;	/* Next PV */

			/* Don't allocate onto the log pv */
			if ((alloc != ALLOC_ANYWHERE) && ah->log_count &&
			    (pvm->pv == ah->log_area.pv))
				continue;	/* Next PV */

			already_found_one = 0;
			/* First area in each list is the largest */
			list_iterate_items(pva, &pvm->areas) {
				if (contiguous) {
					if (prev_lvseg &&
					    _check_contiguous(prev_lvseg,
							      pvm->pv,
							      pva, areas)) {
						contiguous_count++;
						break; /* Next PV */
					}
					continue;
				}

				/* Is it big enough on its own? */
				if ((pva->count < needed - *allocated) &&
				    ((!can_split && !ah->log_count) ||
				     (already_found_one &&
				      !(alloc == ALLOC_ANYWHERE))))
					break;	/* Next PV */

				if (!already_found_one ||
				    alloc == ALLOC_ANYWHERE) {
					ix++;
					already_found_one = 1;
				}

				areas[ix + ix_offset -1] = pva;

				break;	/* Next PV */
			}
			if (ix >= areas_size)
				break;
		}

		if (contiguous && (contiguous_count < ix_offset))
			break;

		if (ix + ix_offset < ah->area_count + ah->log_count)
			/* FIXME With ALLOC_ANYWHERE, need to split areas */
			break;

		/* sort the areas so we allocate from the biggest */
		if (ix > 1)
			qsort(areas + ix_offset, ix, sizeof(*areas),
			      _comp_area);

		if (!_alloc_parallel_area(ah, needed, areas,
					  allocated,
					  ah->log_count ?
						*(areas + ix_offset + ix - 1) :
						NULL)) {
			stack;
			return 0;
		}

	} while (*allocated != needed && can_split);

	return 1;
}

/*
 * Allocate several segments, each the same size, in parallel.
 * If mirrored_pv and mirrored_pe are supplied, it is used as
 * the first area, and additional areas are allocated parallel to it.
 */
static int _allocate(struct alloc_handle *ah,
		     struct volume_group *vg,
			   struct logical_volume *lv, uint32_t status,
			   uint32_t new_extents,
			   struct list *allocatable_pvs,
			   uint32_t stripes, uint32_t mirrors,
			   struct segment_type *segtype,
			   struct physical_volume *mirrored_pv,
			   uint32_t mirrored_pe)
{
	struct pv_area **areas;
	uint32_t allocated = lv ? lv->le_count : 0;
	uint32_t old_allocated;
	struct lv_segment *prev_lvseg = NULL;
	unsigned can_split = 1;	/* Are we allowed more than one segment? */
	int r = 0;
	struct list *pvms;
	uint32_t areas_size;

	if (allocated >= new_extents) {
		log_error("_allocate called with no work to do!");
		return 1;
	}

	if (mirrored_pv || (ah->alloc == ALLOC_CONTIGUOUS) || ah->log_count)
		can_split = 0;

	if (lv && !list_empty(&lv->segments))
		prev_lvseg = list_item(list_last(&lv->segments),
				       struct lv_segment);
	/*
	 * Build the sets of available areas on the pv's.
	 */
	if (!(pvms = create_pv_maps(ah->mem, vg, allocatable_pvs))) {
		stack;
		return 0;
	}

	areas_size = list_size(pvms);
	if (areas_size < ah->area_count + ah->log_count) {
		if (ah->alloc != ALLOC_ANYWHERE) {
			log_error("Not enough PVs with free space available "
				  "for parallel allocation.");
			log_error("Consider --alloc anywhere if desperate.");
			return 0;
		}
		areas_size = ah->area_count + ah->log_count;
	}

	/* Allocate an array of pv_areas to hold the largest space on each PV */
	if (!(areas = dbg_malloc(sizeof(*areas) * areas_size))) {
		log_err("Couldn't allocate areas array.");
		return 0;
	}

	old_allocated = allocated;
	if (!_find_parallel_space(ah, ALLOC_CONTIGUOUS, pvms, areas,
				  areas_size, can_split,
				  prev_lvseg, &allocated, new_extents)) {
		stack;
		goto out;
	}

	if ((allocated == new_extents) || (ah->alloc == ALLOC_CONTIGUOUS) ||
	    (!can_split && (allocated != old_allocated)))
		goto finished;

	old_allocated = allocated;
	if (!_find_parallel_space(ah, ALLOC_NORMAL, pvms, areas,
				  areas_size, can_split,
				  prev_lvseg, &allocated, new_extents)) {
		stack;
		goto out;
	}

	if ((allocated == new_extents) || (ah->alloc == ALLOC_NORMAL) ||
	    (!can_split && (allocated != old_allocated)))
		goto finished;

	if (!_find_parallel_space(ah, ALLOC_ANYWHERE, pvms, areas,
				  areas_size, can_split,
				  prev_lvseg, &allocated, new_extents)) {
		stack;
		goto out;
	}

      finished:
	if (allocated != new_extents) {
		log_error("Insufficient suitable %sallocatable extents "
			  "for logical volume %s: %u more required",
			  can_split ? "" : "contiguous ",
			  lv ? lv->name : "",
			  (new_extents - allocated) * ah->area_count
			  / ah->area_multiple);
		goto out;
	}

	r = 1;

      out:
	dbg_free(areas);
	return r;
}

int lv_add_virtual_segment(struct logical_volume *lv, uint32_t status,
			   uint32_t extents, struct segment_type *segtype)
{
	struct lv_segment *seg;

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv,
				     lv->le_count, extents, status, 0,
				     NULL, 0, extents, 0, 0, 0))) {
		log_error("Couldn't allocate new zero segment.");
		return 0;
	}

	list_add(&lv->segments, &seg->list);

	lv->le_count += extents;
	lv->size += (uint64_t) extents *lv->vg->extent_size;

	lv->status |= VIRTUAL;

	return 1;
}

/*
 * Entry point for all extent allocations.
 */
struct alloc_handle *allocate_extents(struct volume_group *vg,
				      struct logical_volume *lv,
				      struct segment_type *segtype,
				      uint32_t stripes,
				      uint32_t mirrors, uint32_t log_count,
				      uint32_t extents,
				      struct physical_volume *mirrored_pv,
				      uint32_t mirrored_pe,
				      uint32_t status,
				      struct list *allocatable_pvs,
				      alloc_policy_t alloc)
{
	struct alloc_handle *ah;

	if (segtype_is_virtual(segtype)) {
		log_error("allocate_extents does not handle virtual segments");
		return NULL;
	}

	if (vg->fid->fmt->ops->segtype_supported &&
	    !vg->fid->fmt->ops->segtype_supported(vg->fid, segtype)) {
		log_error("Metadata format (%s) does not support required "
			  "LV segment type (%s).", vg->fid->fmt->name,
			  segtype->name);
		log_error("Consider changing the metadata format by running "
			  "vgconvert.");
		return NULL;
	}

	if (alloc == ALLOC_INHERIT)
		alloc = vg->alloc;

	if (!(ah = _alloc_init(vg->cmd->mem, segtype, alloc, mirrors,
			       stripes, log_count, mirrored_pv))) {
		stack;
		return NULL;
	}

	if (!segtype_is_virtual(segtype) &&
	    !_allocate(ah, vg, lv, status, (lv ? lv->le_count : 0) + extents,
		       allocatable_pvs,
		       stripes, mirrors, segtype, mirrored_pv, mirrored_pe)) {
		stack;
		alloc_destroy(ah);
		return NULL;
	}

	return ah;
}

/*
 * Add new segments to an LV from supplied list of areas.
 */
int lv_add_segment(struct alloc_handle *ah,
		   uint32_t first_area, uint32_t num_areas,
		   struct logical_volume *lv,
		   struct segment_type *segtype,
		   uint32_t stripe_size,
		   struct physical_volume *mirrored_pv,
		   uint32_t mirrored_pe,
		   uint32_t status,
		   uint32_t region_size,
		   struct logical_volume *log_lv)
{
	if (segtype_is_virtual(segtype)) {
		log_error("lv_add_segment cannot handle virtual segments");
		return 0;
	}

	if (!_setup_alloced_segments(lv, &ah->alloced_areas[first_area],
				     num_areas, status,
				     stripe_size, segtype,
				     mirrored_pv, mirrored_pe,
				     region_size, log_lv)) {
		stack;
		return 0;
	}

	if ((segtype->flags & SEG_CAN_SPLIT) && !lv_merge_segments(lv)) {
		log_err("Couldn't merge segments after extending "
			"logical volume.");
		return 0;
	}

	if (lv->vg->fid->fmt->ops->lv_setup &&
	    !lv->vg->fid->fmt->ops->lv_setup(lv->vg->fid, lv)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Turn an empty LV into a mirror log.
 */
int lv_add_log_segment(struct alloc_handle *ah, struct logical_volume *log_lv)
{
	struct lv_segment *seg;

	if (list_size(&log_lv->segments)) {
		log_error("Log segments can only be added to an empty LV");
		return 0;
	}

	if (!(seg = alloc_lv_segment(log_lv->vg->cmd->mem,
				     get_segtype_from_string(log_lv->vg->cmd,
							     "striped"),
				     log_lv, 0, ah->log_area.len, MIRROR_LOG,
				     0, NULL, 1, ah->log_area.len, 0, 0, 0))) {
		log_error("Couldn't allocate new mirror log segment.");
		return 0;
	}

	if (!set_lv_segment_area_pv(seg, 0, ah->log_area.pv, ah->log_area.pe)) {
		stack;
		return 0;
	}

	list_add(&log_lv->segments, &seg->list);
	log_lv->le_count += ah->log_area.len;
	log_lv->size += (uint64_t) log_lv->le_count *log_lv->vg->extent_size;

	log_lv->vg->free_count--;

	if (log_lv->vg->fid->fmt->ops->lv_setup &&
	    !log_lv->vg->fid->fmt->ops->lv_setup(log_lv->vg->fid, log_lv)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Entry point for single-step LV allocation + extension.
 */
int lv_extend(struct logical_volume *lv,
	      struct segment_type *segtype,
	      uint32_t stripes, uint32_t stripe_size,
	      uint32_t mirrors, uint32_t extents,
	      struct physical_volume *mirrored_pv, uint32_t mirrored_pe,
	      uint32_t status, struct list *allocatable_pvs,
	      alloc_policy_t alloc)
{
	int r = 1;
	struct alloc_handle *ah;

	if (segtype_is_virtual(segtype))
		return lv_add_virtual_segment(lv, status, extents, segtype);

	if (!(ah = allocate_extents(lv->vg, lv, segtype, stripes, mirrors, 0,
				    extents, mirrored_pv, mirrored_pe, status,
				    allocatable_pvs, alloc))) {
		stack;
		return 0;
	}

	if (!lv_add_segment(ah, 0, ah->area_count, lv, segtype, stripe_size,
			    mirrored_pv, mirrored_pe, status, 0, NULL)) {
		stack;
		goto out;
	}

      out:
	alloc_destroy(ah);
	return r;
}

static char *_generate_lv_name(struct volume_group *vg, const char *format,
			       char *buffer, size_t len)
{
	struct lv_list *lvl;
	int high = -1, i;

	list_iterate_items(lvl, &vg->lvs) {
		if (sscanf(lvl->lv->name, format, &i) != 1)
			continue;

		if (i > high)
			high = i;
	}

	if (lvm_snprintf(buffer, len, format, high + 1) < 0)
		return NULL;

	return buffer;
}

/*
 * Create a new empty LV.
 */
struct logical_volume *lv_create_empty(struct format_instance *fi,
				       const char *name,
				       const char *name_format,
				       union lvid *lvid,
				       uint32_t status,
				       alloc_policy_t alloc,
				       int import,
				       struct volume_group *vg)
{
	struct cmd_context *cmd = vg->cmd;
	struct lv_list *ll = NULL;
	struct logical_volume *lv;
	char dname[32];

	if (vg->max_lv && (vg->max_lv == vg->lv_count)) {
		log_error("Maximum number of logical volumes (%u) reached "
			  "in volume group %s", vg->max_lv, vg->name);
		return NULL;
	}

	if (!name && !(name = _generate_lv_name(vg, name_format, dname,
						sizeof(dname)))) {
		log_error("Failed to generate unique name for the new "
			  "logical volume");
		return NULL;
	}

	if (!import)
		log_verbose("Creating logical volume %s", name);

	if (!(ll = pool_zalloc(cmd->mem, sizeof(*ll))) ||
	    !(ll->lv = pool_zalloc(cmd->mem, sizeof(*ll->lv)))) {
		log_error("lv_list allocation failed");
		if (ll)
			pool_free(cmd->mem, ll);
		return NULL;
	}

	lv = ll->lv;
	lv->vg = vg;

	if (!(lv->name = pool_strdup(cmd->mem, name))) {
		log_error("lv name strdup failed");
		if (ll)
			pool_free(cmd->mem, ll);
		return NULL;
	}

	lv->status = status;
	lv->alloc = alloc;
	lv->read_ahead = 0;
	lv->major = -1;
	lv->minor = -1;
	lv->size = UINT64_C(0);
	lv->le_count = 0;
	lv->snapshot = NULL;
	list_init(&lv->snapshot_segs);
	list_init(&lv->segments);
	list_init(&lv->tags);

	if (lvid)
		lv->lvid = *lvid;

	if (fi->fmt->ops->lv_setup && !fi->fmt->ops->lv_setup(fi, lv)) {
		stack;
		if (ll)
			pool_free(cmd->mem, ll);
		return NULL;
	}

	if (!import)
		vg->lv_count++;

	list_add(&vg->lvs, &ll->list);

	return lv;
}
