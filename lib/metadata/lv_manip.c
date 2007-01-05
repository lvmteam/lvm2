/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
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
#include "locking.h"
#include "pv_map.h"
#include "lvm-string.h"
#include "toolcontext.h"
#include "lv_alloc.h"
#include "pv_alloc.h"
#include "display.h"
#include "segtype.h"

/*
 * PVs used by a segment of an LV
 */
struct seg_pvs {
	struct list list;

	struct list pvs;	/* struct pv_list */

	uint32_t le;
	uint32_t len;
};

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
struct lv_segment *alloc_lv_segment(struct dm_pool *mem,
				    const struct segment_type *segtype,
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
	uint32_t areas_sz = area_count * sizeof(*seg->areas);

	if (!(seg = dm_pool_zalloc(mem, sizeof(*seg)))) {
		stack;
		return NULL;
	}

	if (!(seg->areas = dm_pool_zalloc(mem, areas_sz))) {
		dm_pool_free(mem, seg);
		stack;
		return NULL;
	}

	if (!segtype) {
		log_error("alloc_lv_segment: Missing segtype.");
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
	seg->mirror_seg = NULL;
	list_init(&seg->tags);

	if (log_lv) {
		log_lv->status |= MIRROR_LOG;
		first_seg(log_lv)->mirror_seg = seg;
	}

	return seg;
}

struct lv_segment *alloc_snapshot_seg(struct logical_volume *lv,
				      uint32_t status, uint32_t old_le_count)
{
	struct lv_segment *seg;
	const struct segment_type *segtype;

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

void release_lv_segment_area(struct lv_segment *seg, uint32_t s,
			     uint32_t area_reduction)
{
	if (seg_type(seg, s) == AREA_UNASSIGNED)
		return;

	if (seg_type(seg, s) == AREA_PV) {
		release_pv_segment(seg_pvseg(seg, s), area_reduction);
		return;
	}

	if (seg_lv(seg, s)->status & MIRROR_IMAGE) {
		lv_reduce(seg_lv(seg, s), area_reduction);
		return;
	}

	if (area_reduction == seg->area_len) {
		seg_lv(seg, s) = NULL;
		seg_le(seg, s) = 0;
		seg_type(seg, s) = AREA_UNASSIGNED;
	}
}

/*
 * Move a segment area from one segment to another
 */
int move_lv_segment_area(struct lv_segment *seg_to, uint32_t area_to,
			 struct lv_segment *seg_from, uint32_t area_from)
{
	struct physical_volume *pv;
	struct logical_volume *lv;
	uint32_t pe, le;

	switch (seg_type(seg_from, area_from)) {
	case AREA_PV:
		pv = seg_pv(seg_from, area_from);
		pe = seg_pe(seg_from, area_from);

		release_lv_segment_area(seg_from, area_from,
					seg_from->area_len);
		release_lv_segment_area(seg_to, area_to, seg_to->area_len);

		if (!set_lv_segment_area_pv(seg_to, area_to, pv, pe)) {
			stack;
			return 0;
		}

		break;

	case AREA_LV:
		lv = seg_lv(seg_from, area_from);
		le = seg_le(seg_from, area_from);

		release_lv_segment_area(seg_from, area_from,
					seg_from->area_len);
		release_lv_segment_area(seg_to, area_to, seg_to->area_len);

		set_lv_segment_area_lv(seg_to, area_to, lv, le, 0);

		break;

	case AREA_UNASSIGNED:
		release_lv_segment_area(seg_to, area_to, seg_to->area_len);
	}

	return 1;
}

/*
 * Link part of a PV to an LV segment.
 */
int set_lv_segment_area_pv(struct lv_segment *seg, uint32_t area_num,
			   struct physical_volume *pv, uint32_t pe)
{
	seg->areas[area_num].type = AREA_PV;

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
			    struct logical_volume *lv, uint32_t le,
			    uint32_t flags)
{
	seg->areas[area_num].type = AREA_LV;
	seg_lv(seg, area_num) = lv;
	seg_le(seg, area_num) = le;
	lv->status |= flags;
}

/*
 * Prepare for adding parallel areas to an existing segment.
 */
static int _lv_segment_add_areas(struct logical_volume *lv,
				 struct lv_segment *seg,
				 uint32_t new_area_count)
{
	struct lv_segment_area *newareas;
	uint32_t areas_sz = new_area_count * sizeof(*newareas);

	if (!(newareas = dm_pool_zalloc(lv->vg->cmd->mem, areas_sz))) {
		stack;
		return 0;
	}

	memcpy(newareas, seg->areas, seg->area_count * sizeof(*seg->areas));

	seg->areas = newareas;
	seg->area_count = new_area_count;

	return 1;
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

	for (s = 0; s < seg->area_count; s++)
		release_lv_segment_area(seg, s, area_reduction);

	seg->len -= reduction;
	seg->area_len -= area_reduction;

	return 1;
}

/*
 * Entry point for all LV reductions in size.
 */
static int _lv_reduce(struct logical_volume *lv, uint32_t extents, int delete)
{
	struct lv_list *lvl;
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

	if (!delete)
		return 1;

	/* Remove the LV if it is now empty */
	if (!lv->le_count) {
		if (!(lvl = find_lv_in_vg(lv->vg, lv->name))) {
			stack;
			return 0;
		}

		list_del(&lvl->list);

		lv->vg->lv_count--;
	} else if (lv->vg->fid->fmt->ops->lv_setup &&
		   !lv->vg->fid->fmt->ops->lv_setup(lv->vg->fid, lv)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Empty an LV.
 */
int lv_empty(struct logical_volume *lv)
{
	return _lv_reduce(lv, lv->le_count, 0);
}

/*
 * Remove given number of extents from LV.
 */
int lv_reduce(struct logical_volume *lv, uint32_t extents)
{
	return _lv_reduce(lv, extents, 1);
}

/*
 * Completely remove an LV.
 */
int lv_remove(struct logical_volume *lv)
{

	if (!lv_reduce(lv, lv->le_count)) {
		stack;
		return 0;
	}

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
	struct cmd_context *cmd;
	struct dm_pool *mem;

	alloc_policy_t alloc;		/* Overall policy */
	uint32_t area_count;		/* Number of parallel areas */
	uint32_t area_multiple;		/* seg->len = area_len * area_multiple */
	uint32_t log_count;		/* Number of parallel 1-extent logs */
	uint32_t total_area_len;	/* Total number of parallel extents */

	struct physical_volume *mirrored_pv;	/* FIXME Remove this */
	uint32_t mirrored_pe;			/* FIXME Remove this */
	struct list *parallel_areas;	/* PVs to avoid */

	struct alloced_area log_area;	/* Extent used for log */
	struct list alloced_areas[0];	/* Lists of areas in each stripe */
};

static uint32_t calc_area_multiple(const struct segment_type *segtype,
				   const uint32_t area_count)
{
	if (!segtype_is_striped(segtype) || !area_count)
		return 1;

	return area_count;
}

/*
 * Preparation for a specific allocation attempt
 */
static struct alloc_handle *_alloc_init(struct cmd_context *cmd,
					struct dm_pool *mem,
					const struct segment_type *segtype,
					alloc_policy_t alloc,
					uint32_t mirrors,
					uint32_t stripes,
					uint32_t log_count,
					struct physical_volume *mirrored_pv,
					uint32_t mirrored_pe,
					struct list *parallel_areas)
{
	struct alloc_handle *ah;
	uint32_t s, area_count;

	if (stripes > 1 && mirrors > 1) {
		log_error("Striped mirrors are not supported yet");
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

	if (!(ah = dm_pool_zalloc(mem, sizeof(*ah) + sizeof(ah->alloced_areas[0]) * area_count))) {
		log_error("allocation handle allocation failed");
		return NULL;
	}

	if (segtype_is_virtual(segtype))
		return ah;

	ah->cmd = cmd;

	if (!(ah->mem = dm_pool_create("allocation", 1024))) {
		log_error("allocation pool creation failed");
		return NULL;
	}

	ah->area_count = area_count;
	ah->log_count = log_count;
	ah->alloc = alloc;
	ah->area_multiple = calc_area_multiple(segtype, area_count);

	for (s = 0; s < ah->area_count; s++)
		list_init(&ah->alloced_areas[s]);

	ah->mirrored_pv = mirrored_pv;
	ah->mirrored_pe = mirrored_pe;
	ah->parallel_areas = parallel_areas;

	return ah;
}

void alloc_destroy(struct alloc_handle *ah)
{
	if (ah->mem)
		dm_pool_destroy(ah->mem);
}

static int _log_parallel_areas(struct dm_pool *mem, struct list *parallel_areas)
{
	struct seg_pvs *spvs;
	struct pv_list *pvl;
	char *pvnames;

	if (!parallel_areas)
		return 1;

	if (!dm_pool_begin_object(mem, 256)) {
		log_error("dm_pool_begin_object failed");
		return 0;
	}

	list_iterate_items(spvs, parallel_areas) {
		list_iterate_items(pvl, &spvs->pvs) {
			if (!dm_pool_grow_object(mem, dev_name(pvl->pv->dev), strlen(dev_name(pvl->pv->dev)))) {
				log_error("dm_pool_grow_object failed");
				dm_pool_abandon_object(mem);
				return 0;
			}
			if (!dm_pool_grow_object(mem, " ", 1)) {
				log_error("dm_pool_grow_object failed");
				dm_pool_abandon_object(mem);
				return 0;
			}
		}

		if (!dm_pool_grow_object(mem, "\0", 1)) {
			log_error("dm_pool_grow_object failed");
			dm_pool_abandon_object(mem);
			return 0;
		}

		pvnames = dm_pool_end_object(mem);
		log_debug("Parallel PVs at LE %" PRIu32 " length %" PRIu32 ": %s",
			  spvs->le, spvs->len, pvnames);
		dm_pool_free(mem, pvnames);
	}

	return 1;
}

static int _setup_alloced_segment(struct logical_volume *lv, uint32_t status,
				  uint32_t area_count,
				  uint32_t stripe_size,
				  const struct segment_type *segtype,
				  struct alloced_area *aa,
				  struct physical_volume *mirrored_pv,
				  uint32_t mirrored_pe,
				  uint32_t region_size,
				  struct logical_volume *log_lv __attribute((unused)))
{
	uint32_t s, extents, area_multiple, extra_areas = 0;
	struct lv_segment *seg;

	if (mirrored_pv)
		extra_areas = 1;

	area_multiple = calc_area_multiple(segtype, area_count);

	/* log_lv gets set up elsehere */
	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv,
				     lv->le_count,
				     aa[0].len * area_multiple,
				     status, stripe_size, NULL,
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

	if (segtype_is_mirrored(segtype))
		lv->status |= MIRRORED;

	return 1;
}

static int _setup_alloced_segments(struct logical_volume *lv,
				   struct list *alloced_areas,
				   uint32_t area_count,
				   uint32_t status,
				   uint32_t stripe_size,
				   const struct segment_type *segtype,
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
	uint32_t area_len, remaining;
	uint32_t s;
	struct alloced_area *aa;

	remaining = needed - *ix;
	area_len = remaining / ah->area_multiple;

	/* Reduce area_len to the smallest of the areas */
	for (s = 0; s < ah->area_count; s++)
		if (area_len > areas[s]->count)
			area_len = areas[s]->count;

	if (!(aa = dm_pool_alloc(ah->mem, sizeof(*aa) *
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

	ah->total_area_len += area_len;

	for (s = 0; s < ah->area_count; s++)
		consume_pv_area(areas[s], area_len);

	if (log_area) {
		ah->log_area.pv = log_area->map->pv;
		ah->log_area.pe = log_area->start;
		ah->log_area.len = MIRROR_LOG_SIZE;	/* FIXME Calculate & check this */
		consume_pv_area(log_area, ah->log_area.len);
	}

	*ix += area_len * ah->area_multiple;

	return 1;
}

/*
 * Call fn for each AREA_PV used by the LV segment at lv:le of length *max_seg_len.
 * If any constituent area contains more than one segment, max_seg_len is
 * reduced to cover only the first.
 * fn should return 0 on error, 1 to continue scanning or >1 to terminate without error.
 * In the last case, this function passes on the return code.
 */
static int _for_each_pv(struct cmd_context *cmd, struct logical_volume *lv,
			uint32_t le, uint32_t len, uint32_t *max_seg_len,
			uint32_t first_area, uint32_t max_areas,
			int top_level_area_index,
			int only_single_area_segments,
			int (*fn)(struct cmd_context *cmd,
				  struct pv_segment *peg, uint32_t s,
				  void *data),
			void *data)
{
	struct lv_segment *seg;
	uint32_t s;
	uint32_t remaining_seg_len, area_len, area_multiple;
	int r = 1;

	if (!(seg = find_seg_by_le(lv, le))) {
		log_error("Failed to find segment for %s extent %" PRIu32,
			  lv->name, le);
		return 0;
	}

	/* Remaining logical length of segment */
	remaining_seg_len = seg->len - (le - seg->le);

	if (remaining_seg_len > len)
		remaining_seg_len = len;

	if (max_seg_len && *max_seg_len > remaining_seg_len)
		*max_seg_len = remaining_seg_len;

	area_multiple = calc_area_multiple(seg->segtype, seg->area_count);
	area_len = remaining_seg_len / area_multiple ? : 1;

	for (s = first_area;
	     s < seg->area_count && (!max_areas || s <= max_areas);
	     s++) {
		if (seg_type(seg, s) == AREA_LV) {
			if (!(r = _for_each_pv(cmd, seg_lv(seg, s),
					       seg_le(seg, s) +
					       (le - seg->le) / area_multiple,
					       area_len, max_seg_len,
					       only_single_area_segments ? 0 : 0,
					       only_single_area_segments ? 1 : 0,
					       top_level_area_index != -1 ? top_level_area_index : s,
					       only_single_area_segments, fn,
					       data)))
				stack;
		} else if (seg_type(seg, s) == AREA_PV)
			if (!(r = fn(cmd, seg_pvseg(seg, s), top_level_area_index != -1 ? top_level_area_index : s, data)))
				stack;
		if (r != 1)
			return r;
	}

	/* FIXME only_single_area_segments used as workaround to skip log LV - needs new param? */
	if (!only_single_area_segments && seg_is_mirrored(seg) && seg->log_lv) {
		if (!(r = _for_each_pv(cmd, seg->log_lv, 0, MIRROR_LOG_SIZE,
				       NULL, 0, 0, 0, only_single_area_segments,
				       fn, data)))
			stack;
		if (r != 1)
			return r;
	}

	/* FIXME Add snapshot cow LVs etc. */

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
 * Search for pvseg that matches condition
 */
struct pv_match {
	int (*condition)(struct pv_segment *pvseg, struct pv_area *pva);

	struct pv_area **areas;
	struct pv_area *pva;
	uint32_t areas_size;
	int s;	/* Area index of match */
};

/*
 * Is PV area on the same PV?
 */
static int _is_same_pv(struct pv_segment *pvseg, struct pv_area *pva)
{
	if (pvseg->pv != pva->map->pv)
		return 0;

	return 1;
}

/*
 * Is PV area contiguous to PV segment?
 */
static int _is_contiguous(struct pv_segment *pvseg, struct pv_area *pva)
{
	if (pvseg->pv != pva->map->pv)
		return 0;

	if (pvseg->pe + pvseg->len != pva->start)
		return 0;

	return 1;
}

static int _is_condition(struct cmd_context *cmd,
			 struct pv_segment *pvseg, uint32_t s,
			 void *data)
{
	struct pv_match *pvmatch = data;

	if (!pvmatch->condition(pvseg, pvmatch->pva))
		return 1;	/* Continue */

	if (s >= pvmatch->areas_size)
		return 1;

	pvmatch->areas[s] = pvmatch->pva;

	return 2;	/* Finished */
}

/*
 * Is pva on same PV as any existing areas?
 */
static int _check_cling(struct cmd_context *cmd,
			struct lv_segment *prev_lvseg, struct pv_area *pva,
			struct pv_area **areas, uint32_t areas_size)
{
	struct pv_match pvmatch;
	int r;

	pvmatch.condition = _is_same_pv;
	pvmatch.areas = areas;
	pvmatch.areas_size = areas_size;
	pvmatch.pva = pva;

	/* FIXME Cope with stacks by flattening */
	if (!(r = _for_each_pv(cmd, prev_lvseg->lv,
			       prev_lvseg->le + prev_lvseg->len - 1, 1, NULL,
			       0, 0, -1, 1,
			       _is_condition, &pvmatch)))
		stack;

	if (r != 2)
		return 0;

	return 1;
}

/*
 * Is pva contiguous to any existing areas or on the same PV?
 */
static int _check_contiguous(struct cmd_context *cmd,
			     struct lv_segment *prev_lvseg, struct pv_area *pva,
			     struct pv_area **areas, uint32_t areas_size)
{
	struct pv_match pvmatch;
	int r;

	pvmatch.condition = _is_contiguous;
	pvmatch.areas = areas;
	pvmatch.areas_size = areas_size;
	pvmatch.pva = pva;

	/* FIXME Cope with stacks by flattening */
	if (!(r = _for_each_pv(cmd, prev_lvseg->lv,
			       prev_lvseg->le + prev_lvseg->len - 1, 1, NULL,
			       0, 0, -1, 1,
			       _is_condition, &pvmatch)))
		stack;

	if (r != 2)
		return 0;

	return 1;
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
	struct pv_list *pvl;
	unsigned already_found_one = 0;
	unsigned contiguous = 0, cling = 0, preferred_count = 0;
	unsigned ix;
	unsigned ix_offset = 0;	/* Offset for non-preferred allocations */
	uint32_t max_parallel;	/* Maximum extents to allocate */
	uint32_t next_le;
	struct seg_pvs *spvs;
	struct list *parallel_pvs;
	uint32_t free_pes;

	/* Is there enough total space? */
	free_pes = pv_maps_size(pvms);
	if (needed - *allocated > free_pes) {
		log_error("Insufficient free space: %" PRIu32 " extents needed,"
			  " but only %" PRIu32 " available",
			  needed - *allocated, free_pes);
		return 0;
	}

	/* FIXME Select log PV appropriately if there isn't one yet */

	/* Are there any preceding segments we must follow on from? */
	if (prev_lvseg) {
		ix_offset = prev_lvseg->area_count;
		if ((alloc == ALLOC_CONTIGUOUS))
			contiguous = 1;
		else if ((alloc == ALLOC_CLING))
			cling = 1;
		else
			ix_offset = 0;
	}

	/* FIXME This algorithm needs a lot of cleaning up! */
	/* FIXME anywhere doesn't find all space yet */
	/* ix_offset holds the number of allocations that must be contiguous */
	/* ix holds the number of areas found on other PVs */
	do {
		ix = 0;
		preferred_count = 0;

		parallel_pvs = NULL;
		max_parallel = needed;

		/*
		 * If there are existing parallel PVs, avoid them and reduce
		 * the maximum we can allocate in one go accordingly.
		 */
		if (ah->parallel_areas) {
			next_le = (prev_lvseg ? prev_lvseg->le + prev_lvseg->len : 0) + *allocated / ah->area_multiple;
			list_iterate_items(spvs, ah->parallel_areas) {
				if (next_le >= spvs->le + spvs->len)
					continue;

				if (max_parallel > (spvs->le + spvs->len) * ah->area_multiple)
					max_parallel = (spvs->le + spvs->len) * ah->area_multiple;
				parallel_pvs = &spvs->pvs;
				break;
			}
		}

		/*
		 * Put the smallest area of each PV that is at least the
		 * size we need into areas array.  If there isn't one
		 * that fits completely and we're allowed more than one
		 * LV segment, then take the largest remaining instead.
		 */
		list_iterate_items(pvm, pvms) {
			if (list_empty(&pvm->areas))
				continue;	/* Next PV */

			if (alloc != ALLOC_ANYWHERE) {
				/* Don't allocate onto the log pv */
				if (ah->log_count &&
				    pvm->pv == ah->log_area.pv)
					continue;	/* Next PV */

				/* Avoid PVs used by existing parallel areas */
				if (parallel_pvs)
					list_iterate_items(pvl, parallel_pvs)
						if (pvm->pv == pvl->pv)
							goto next_pv;
			}

			already_found_one = 0;
			/* First area in each list is the largest */
			list_iterate_items(pva, &pvm->areas) {
				if (contiguous) {
					if (prev_lvseg &&
					    _check_contiguous(ah->cmd,
							      prev_lvseg,
							      pva, areas,
							      areas_size)) {
						preferred_count++;
						goto next_pv;
					}
					continue;
				}

				if (cling) {
					if (prev_lvseg &&
					    _check_cling(ah->cmd,
							   prev_lvseg,
							   pva, areas,
							   areas_size)) {
						preferred_count++;
					}
					goto next_pv;
				}

				/* Is it big enough on its own? */
				if (pva->count * ah->area_multiple <
				    max_parallel - *allocated &&
				    ((!can_split && !ah->log_count) ||
				     (already_found_one &&
				      !(alloc == ALLOC_ANYWHERE))))
					goto next_pv;

				if (!already_found_one ||
				    alloc == ALLOC_ANYWHERE) {
					ix++;
					already_found_one = 1;
				}

				areas[ix + ix_offset - 1] = pva;

				goto next_pv;
			}
		next_pv:
			if (ix >= areas_size)
				break;
		}

		if ((contiguous || cling) && (preferred_count < ix_offset))
			break;

		/* Only allocate log_area the first time around */
		if (ix + ix_offset < ah->area_count +
			    ((ah->log_count && !ah->log_area.len) ?
				ah->log_count : 0))
			/* FIXME With ALLOC_ANYWHERE, need to split areas */
			break;

		/* sort the areas so we allocate from the biggest */
		if (ix > 1)
			qsort(areas + ix_offset, ix, sizeof(*areas),
			      _comp_area);

		/* First time around, use smallest area as log_area */
		/* FIXME decide which PV to use at top of function instead */
		if (!_alloc_parallel_area(ah, max_parallel, areas,
					  allocated,
					  (ah->log_count && !ah->log_area.len) ?
						*(areas + ix_offset + ix - 1) :
						NULL)) {
			stack;
			return 0;
		}

	} while (!contiguous && *allocated != needed && can_split);

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
		     const struct segment_type *segtype)
{
	struct pv_area **areas;
	uint32_t allocated = lv ? lv->le_count : 0;
	uint32_t old_allocated;
	struct lv_segment *prev_lvseg = NULL;
	unsigned can_split = 1;	/* Are we allowed more than one segment? */
	int r = 0;
	struct list *pvms;
	uint32_t areas_size;
	alloc_policy_t alloc;

	if (allocated >= new_extents && !ah->log_count) {
		log_error("_allocate called with no work to do!");
		return 1;
	}

	if (ah->mirrored_pv || (ah->alloc == ALLOC_CONTIGUOUS))
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

	if (!_log_parallel_areas(ah->mem, ah->parallel_areas))
		stack;

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

	/* Upper bound if none of the PVs in prev_lvseg is in pvms */
	/* FIXME Work size out properly */
	if (prev_lvseg)
		areas_size += prev_lvseg->area_count;

	/* Allocate an array of pv_areas to hold the largest space on each PV */
	if (!(areas = dm_malloc(sizeof(*areas) * areas_size))) {
		log_err("Couldn't allocate areas array.");
		return 0;
	}

	/* Attempt each defined allocation policy in turn */
	for (alloc = ALLOC_CONTIGUOUS; alloc < ALLOC_INHERIT; alloc++) {
		old_allocated = allocated;
		if (!_find_parallel_space(ah, alloc, pvms, areas,
					  areas_size, can_split,
					  prev_lvseg, &allocated, new_extents))
			goto_out;
		if ((allocated == new_extents) || (ah->alloc == alloc) ||
		    (!can_split && (allocated != old_allocated)))
			break;
	}

	if (allocated != new_extents) {
		log_error("Insufficient suitable %sallocatable extents "
			  "for logical volume %s: %u more required",
			  can_split ? "" : "contiguous ",
			  lv ? lv->name : "",
			  (new_extents - allocated) * ah->area_count
			  / ah->area_multiple);
		goto out;
	}

	if (ah->log_count && !ah->log_area.len) {
		log_error("Insufficient extents for log allocation "
			  "for logical volume %s.",
			  lv ? lv->name : "");
		goto out;
	}

	r = 1;

      out:
	dm_free(areas);
	return r;
}

int lv_add_virtual_segment(struct logical_volume *lv, uint32_t status,
			   uint32_t extents, const struct segment_type *segtype)
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
				      const struct segment_type *segtype,
				      uint32_t stripes,
				      uint32_t mirrors, uint32_t log_count,
				      uint32_t extents,
				      struct physical_volume *mirrored_pv,
				      uint32_t mirrored_pe,
				      uint32_t status,
				      struct list *allocatable_pvs,
				      alloc_policy_t alloc,
				      struct list *parallel_areas)
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

	if (!(ah = _alloc_init(vg->cmd, vg->cmd->mem, segtype, alloc, mirrors,
			       stripes, log_count, mirrored_pv,
			       mirrored_pe, parallel_areas))) {
		stack;
		return NULL;
	}

	if (!segtype_is_virtual(segtype) &&
	    !_allocate(ah, vg, lv, status, (lv ? lv->le_count : 0) + extents,
		       allocatable_pvs, stripes, mirrors, segtype)) {
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
		   const struct segment_type *segtype,
		   uint32_t stripe_size,
		   struct physical_volume *mirrored_pv,
		   uint32_t mirrored_pe,
		   uint32_t status,
		   uint32_t region_size,
		   struct logical_volume *log_lv)
{
	if (!segtype) {
		log_error("Missing segtype in lv_add_segment().");
		return 0;
	}

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

	if (log_lv->vg->fid->fmt->ops->lv_setup &&
	    !log_lv->vg->fid->fmt->ops->lv_setup(log_lv->vg->fid, log_lv)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Add a mirror segment
 */
int lv_add_mirror_segment(struct alloc_handle *ah,
			  struct logical_volume *lv,
			  struct logical_volume **sub_lvs,
			  uint32_t mirrors,
			  const struct segment_type *segtype,
			  uint32_t status,
			  uint32_t region_size,
			  struct logical_volume *log_lv)
{
	struct lv_segment *seg;
	uint32_t m;

	if (log_lv && list_empty(&log_lv->segments)) {
		log_error("Log LV %s is empty.", log_lv->name);
		return 0;
	}

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem,
				     get_segtype_from_string(lv->vg->cmd,
							     "mirror"),
				     lv, lv->le_count, ah->total_area_len, 0,
				     0, log_lv, mirrors, ah->total_area_len, 0,
				     region_size, 0))) {
		log_error("Couldn't allocate new mirror segment.");
		return 0;
	}

	for (m = 0; m < mirrors; m++) {
		set_lv_segment_area_lv(seg, m, sub_lvs[m], 0, MIRROR_IMAGE);
		first_seg(sub_lvs[m])->mirror_seg = seg;
	}

	list_add(&lv->segments, &seg->list);
	lv->le_count += ah->total_area_len;
	lv->size += (uint64_t) lv->le_count *lv->vg->extent_size;

	if (lv->vg->fid->fmt->ops->lv_setup &&
	    !lv->vg->fid->fmt->ops->lv_setup(lv->vg->fid, lv)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Add parallel areas to an existing mirror
 */
int lv_add_more_mirrored_areas(struct logical_volume *lv,
			       struct logical_volume **sub_lvs,
			       uint32_t num_extra_areas,
			       uint32_t status)
{
	struct lv_segment *seg;
	uint32_t old_area_count, new_area_count;
	uint32_t m;

	if (list_size(&lv->segments) != 1) {
		log_error("Mirrored LV must only have one segment.");
		return 0;
	}

	seg = first_seg(lv);

	old_area_count = seg->area_count;
	new_area_count = old_area_count + num_extra_areas;

	if (!_lv_segment_add_areas(lv, seg, new_area_count)) {
		log_error("Failed to allocate widened LV segment for %s.",
			  lv->name);
		return 0;
	}

	for (m = old_area_count; m < new_area_count; m++) {
		set_lv_segment_area_lv(seg, m, sub_lvs[m - old_area_count], 0, status);
		first_seg(sub_lvs[m - old_area_count])->mirror_seg = seg;
	}

	return 1;
}

/*
 * Entry point for single-step LV allocation + extension.
 */
int lv_extend(struct logical_volume *lv,
	      const struct segment_type *segtype,
	      uint32_t stripes, uint32_t stripe_size,
	      uint32_t mirrors, uint32_t extents,
	      struct physical_volume *mirrored_pv, uint32_t mirrored_pe,
	      uint32_t status, struct list *allocatable_pvs,
	      alloc_policy_t alloc)
{
	int r = 1;
	uint32_t m;
	struct alloc_handle *ah;
	struct lv_segment *seg;

	if (segtype_is_virtual(segtype))
		return lv_add_virtual_segment(lv, status, extents, segtype);

	if (!(ah = allocate_extents(lv->vg, lv, segtype, stripes, mirrors, 0,
				    extents, mirrored_pv, mirrored_pe, status,
				    allocatable_pvs, alloc, NULL))) {
		stack;
		return 0;
	}

	if (mirrors < 2) {
		if (!lv_add_segment(ah, 0, ah->area_count, lv, segtype, stripe_size,
			    mirrored_pv, mirrored_pe, status, 0, NULL)) {
			stack;
			goto out;
		}
	} else {
		seg = first_seg(lv);
		for (m = 0; m < mirrors; m++) {
			if (!lv_add_segment(ah, m, 1, seg_lv(seg, m),
					    get_segtype_from_string(lv->vg->cmd,
								    "striped"),
					    0, NULL, 0, 0, 0, NULL)) {
				log_error("Aborting. Failed to extend %s.",
					  seg_lv(seg, m)->name);
				return 0;
			}
		}
		seg->area_len += extents;
		seg->len += extents;
		lv->le_count += extents;
		lv->size += (uint64_t) extents *lv->vg->extent_size;
	}

      out:
	alloc_destroy(ah);
	return r;
}

char *generate_lv_name(struct volume_group *vg, const char *format,
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

	if (dm_snprintf(buffer, len, format, high + 1) < 0)
		return NULL;

	return buffer;
}

/*
 * Create a new empty LV.
 */
struct logical_volume *lv_create_empty(struct format_instance *fi,
				       const char *name,
				       union lvid *lvid,
				       uint32_t status,
				       alloc_policy_t alloc,
				       int import,
				       struct volume_group *vg)
{
	struct cmd_context *cmd = vg->cmd;
	struct lv_list *ll = NULL;
	struct logical_volume *lv;
	char dname[NAME_LEN];

	if (vg->max_lv && (vg->max_lv == vg->lv_count)) {
		log_error("Maximum number of logical volumes (%u) reached "
			  "in volume group %s", vg->max_lv, vg->name);
		return NULL;
	}

	if (strstr(name, "%d") &&
	    !(name = generate_lv_name(vg, name, dname, sizeof(dname)))) {
		log_error("Failed to generate unique name for the new "
			  "logical volume");
		return NULL;
	}

	if (!import)
		log_verbose("Creating logical volume %s", name);

	if (!(ll = dm_pool_zalloc(cmd->mem, sizeof(*ll))) ||
	    !(ll->lv = dm_pool_zalloc(cmd->mem, sizeof(*ll->lv)))) {
		log_error("lv_list allocation failed");
		if (ll)
			dm_pool_free(cmd->mem, ll);
		return NULL;
	}

	lv = ll->lv;
	lv->vg = vg;

	if (!(lv->name = dm_pool_strdup(cmd->mem, name))) {
		log_error("lv name strdup failed");
		if (ll)
			dm_pool_free(cmd->mem, ll);
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
			dm_pool_free(cmd->mem, ll);
		return NULL;
	}

	if (!import)
		vg->lv_count++;

	list_add(&vg->lvs, &ll->list);

	return lv;
}

static int _add_pvs(struct cmd_context *cmd, struct pv_segment *peg,
		    uint32_t s __attribute((unused)), void *data)
{
	struct seg_pvs *spvs = (struct seg_pvs *) data;
	struct pv_list *pvl;

	/* Don't add again if it's already on list. */
	list_iterate_items(pvl, &spvs->pvs)
		if (pvl->pv == peg->pv)
			return 1;

	if (!(pvl = dm_pool_alloc(cmd->mem, sizeof(*pvl)))) {
		log_error("pv_list allocation failed");
		return 0;
	}

	pvl->pv = peg->pv;

	list_add(&spvs->pvs, &pvl->list);

	return 1;
}

/*
 * Construct list of segments of LVs showing which PVs they use.
 */
struct list *build_parallel_areas_from_lv(struct cmd_context *cmd,
					  struct logical_volume *lv)
{
	struct list *parallel_areas;
	struct seg_pvs *spvs;
	uint32_t current_le = 0;

	if (!(parallel_areas = dm_pool_alloc(cmd->mem, sizeof(*parallel_areas)))) {
		log_error("parallel_areas allocation failed");
		return NULL;
	}

	list_init(parallel_areas);

	do {
		if (!(spvs = dm_pool_zalloc(cmd->mem, sizeof(*spvs)))) {
			log_error("allocation failed");
			return NULL;
		}

		list_init(&spvs->pvs);

		spvs->le = current_le;
		spvs->len = lv->le_count - current_le;

		list_add(parallel_areas, &spvs->list);

		/* Find next segment end */
		/* FIXME Unnecessary nesting! */
		if (!_for_each_pv(cmd, lv, current_le, spvs->len, &spvs->len,
				  0, 0, -1, 0, _add_pvs, (void *) spvs)) {
			stack;
			return NULL;
		}

		current_le = spvs->le + spvs->len;
	} while (current_le < lv->le_count);

	/* FIXME Merge adjacent segments with identical PV lists (avoids need for contiguous allocation attempts between successful allocations) */

	return parallel_areas;
}
