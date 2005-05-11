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
 * These functions adjust the pe counts in pv's
 * after we've added or removed segments.
 */
static void _get_extents(struct lv_segment *seg)
{
	unsigned int s, count;
	struct physical_volume *pv;

	for (s = 0; s < seg->area_count; s++) {
		if (seg->area[s].type != AREA_PV)
			continue;

		pv = seg->area[s].u.pv.pvseg->pv;
		count = seg->area_len;
		pv->pe_alloc_count += count;
	}
}

static void _put_extents(struct lv_segment *seg)
{
	unsigned int s, count;
	struct physical_volume *pv;

	for (s = 0; s < seg->area_count; s++) {
		if (seg->area[s].type != AREA_PV)
			continue;

		pv = seg->area[s].u.pv.pvseg->pv;

		if (pv) {
			count = seg->area_len;
			assert(pv->pe_alloc_count >= count);
			pv->pe_alloc_count -= count;
		}
	}
}

struct lv_segment *alloc_lv_segment(struct pool *mem,
				    struct segment_type *segtype,
				    struct logical_volume *lv,
				    uint32_t le, uint32_t len,
				    uint32_t status,
				    uint32_t stripe_size,
				    uint32_t area_count,
				    uint32_t area_len,
				    uint32_t chunk_size,
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
	seg->extents_copied = extents_copied;
	list_init(&seg->tags);

	return seg;
}

int set_lv_segment_area_pv(struct lv_segment *seg, uint32_t area_num,
			   struct physical_volume *pv, uint32_t pe)
{
	seg->area[area_num].type = AREA_PV;

	if (!(seg->area[area_num].u.pv.pvseg =
	      assign_peg_to_lvseg(pv, pe, seg->area_len, seg, area_num))) {
		stack;
		return 0;
	}

	return 1;
}

void set_lv_segment_area_lv(struct lv_segment *seg, uint32_t area_num,
			    struct logical_volume *lv, uint32_t le)
{
	seg->area[area_num].type = AREA_LV;
	seg->area[area_num].u.lv.lv = lv;
	seg->area[area_num].u.lv.le = le;
}

static void _shrink_lv_segment(struct lv_segment *seg)
{
	uint32_t s;

	for (s = 0; s < seg->area_count; s++) {
		if (seg->area[s].type != AREA_PV)
			continue;
		release_pv_segment(seg->area[s].u.pv.pvseg, seg->area_len);
	}
}

static int _alloc_parallel_area(struct logical_volume *lv, uint32_t area_count,
				uint32_t stripe_size,
				struct segment_type *segtype,
				struct pv_area **areas, uint32_t *ix)
{
	uint32_t count, area_len, smallest;
	uint32_t s;
	struct lv_segment *seg;
	int striped = 0;

	/* Striped or mirrored? */
	if (seg_is_striped(seg))
		striped = 1;

	count = lv->le_count - *ix;
	area_len = count / (striped ? area_count : 1);
	smallest = areas[area_count - 1]->count;

	if (smallest < area_len)
		area_len = smallest;

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv, *ix,
				     area_len * (striped ? area_count : 1),
				     0u, stripe_size, area_count, area_len,
				     0u, 0u))) {
		log_error("Couldn't allocate new parallel segment.");
		return 0;
	}

	for (s = 0; s < area_count; s++) {
		struct pv_area *pva = areas[s];
		if (!set_lv_segment_area_pv(seg, s, pva->map->pv, pva->start)) {
			stack;
			return 0;
		}
		consume_pv_area(pva, area_len);
	}

	list_add(&lv->segments, &seg->list);
	*ix += seg->len;

	if (!striped)
		lv->status |= MIRRORED;

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

static int _alloc_parallel(struct logical_volume *lv,
			   struct list *pvms, uint32_t allocated,
			   uint32_t stripes, uint32_t stripe_size,
			   uint32_t mirrors, struct segment_type *segtype)
{
	int r = 0;
	struct pv_area **areas;
	unsigned int pv_count, ix;
	struct pv_map *pvm;
	size_t len;
	uint32_t area_count;

	if (stripes > 1 && mirrors > 1) {
		log_error("striped mirrors are not supported yet");
		return 0;
	}

	if (stripes > 1)
		area_count = stripes;
	else
		area_count = mirrors;

	pv_count = list_size(pvms);

	/* allocate an array of pv_areas, one candidate per pv */
	len = sizeof(*areas) * pv_count;
	if (!(areas = dbg_malloc(sizeof(*areas) * pv_count))) {
		log_err("Couldn't allocate areas array.");
		return 0;
	}

	while (allocated != lv->le_count) {

		ix = 0;
		list_iterate_items(pvm, pvms) {
			if (list_empty(&pvm->areas))
				continue;

			areas[ix++] = list_item(pvm->areas.n, struct pv_area);
		}

		if (ix < area_count) {
			log_error("Insufficient allocatable extents suitable "
				  "for parallel use for logical volume "
				  "%s: %u required", lv->name, lv->le_count);
			goto out;
		}

		/* sort the areas so we allocate from the biggest */
		qsort(areas, ix, sizeof(*areas), _comp_area);

		if (!_alloc_parallel_area(lv, area_count, stripe_size, segtype,
					  areas, &allocated)) {
			stack;
			goto out;
		}
	}
	r = 1;

      out:
	dbg_free(areas);
	return r;
}

/*
 * The heart of the allocation code.  This function takes a
 * pv_area and allocates it to the lv.  If the lv doesn't need
 * the complete area then the area is split, otherwise the area
 * is unlinked from the pv_map.
 */
static int _alloc_linear_area(struct logical_volume *lv, uint32_t *ix,
			      struct pv_map *map, struct pv_area *pva)
{
	uint32_t count, remaining;
	struct lv_segment *seg;
	struct segment_type *segtype;

	count = pva->count;
	remaining = lv->le_count - *ix;
	if (count > remaining)
		count = remaining;

	if (!(segtype = get_segtype_from_string(lv->vg->cmd, "striped"))) {
		stack;
		return 0;
	}

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv, *ix,
				     count, 0, 0, 1, count, 0, 0))) {
		log_error("Couldn't allocate new stripe segment.");
		return 0;
	}

	if (!set_lv_segment_area_pv(seg, 0, map->pv, pva->start)) {
		stack;
		return 0;
	}

	list_add(&lv->segments, &seg->list);

	consume_pv_area(pva, seg->len);
	*ix += seg->len;

	return 1;
}

static int _alloc_mirrored_area(struct logical_volume *lv, uint32_t *ix,
				struct pv_map *map, struct pv_area *pva,
				struct segment_type *segtype,
				struct physical_volume *mirrored_pv,
				uint32_t mirrored_pe)
{
	uint32_t count, remaining;
	struct lv_segment *seg;

	count = pva->count;
	remaining = lv->le_count - *ix;
	if (count > remaining)
		count = remaining;

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv, *ix,
				     count, 0, 0, 2, count, 0, 0))) {
		log_err("Couldn't allocate new mirrored segment.");
		return 0;
	}

	/* FIXME Remove AREA_PV restriction here? */
	if (!set_lv_segment_area_pv(seg, 0, mirrored_pv, mirrored_pe) ||
	    !set_lv_segment_area_pv(seg, 1, map->pv, pva->start)) {
		stack;
		return 0;
	}

	list_add(&lv->segments, &seg->list);

	consume_pv_area(pva, seg->len);
	*ix += seg->len;

	return 1;
}

/*
 * Only one area per pv is allowed, so we search
 * for the biggest area, or the first area that
 * can complete the allocation.
 */

static int _alloc_contiguous(struct logical_volume *lv,
			     struct list *pvms, uint32_t allocated)
{
	struct pv_map *pvm;
	struct pv_area *pva;
	struct lv_segment *prev_lvseg;
	struct pv_segment *prev_pvseg = NULL;
	uint32_t largest = 0;

	/* So far the only case is exactly one area */
	if ((prev_lvseg = list_item(list_last(&lv->segments), struct lv_segment)) &&
	    (prev_lvseg->area_count == 1) &&
	    (prev_lvseg->area[0].type == AREA_PV))
		prev_pvseg = prev_lvseg->area[0].u.pv.pvseg;

	list_iterate_items(pvm, pvms) {
		if (prev_pvseg && (prev_pvseg->pv != pvm->pv))
			continue;

		list_iterate_items(pva, &pvm->areas) {
			if (prev_pvseg &&
			    (prev_pvseg->pe + prev_pvseg->len != pva->start))
				continue;

			if (pva->count > largest)
				largest = pva->count;

			/* first item in the list is the biggest */
			if (pva->count < lv->le_count - allocated)
				goto next_pv;

			if (!_alloc_linear_area(lv, &allocated, pvm, pva)) {
				stack;
				return 0;
			}

			goto out;
		}

      next_pv:
		;
	}

      out:
	if (allocated != lv->le_count) {
		log_error("Insufficient allocatable extents (%u) "
			  "for logical volume %s: %u required",
			  largest, lv->name, lv->le_count - allocated);
		return 0;
	}

	return 1;
}

/* FIXME Contiguous depends on *segment* (i.e. stripe) not LV */
static int _alloc_mirrored(struct logical_volume *lv,
			   struct list *pvms, uint32_t allocated,
			   struct segment_type *segtype,
			   struct physical_volume *mirrored_pv,
			   uint32_t mirrored_pe)
{
	struct pv_map *pvm;
	struct pv_area *pva;
	uint32_t max_found = 0;

	/* Try each PV in turn */
	list_iterate_items(pvm, pvms) {
		if (list_empty(&pvm->areas))
			continue;

		/* first item in the list is the biggest */
		pva = list_item(pvm->areas.n, struct pv_area);
		if (pva->count < lv->le_count - allocated) {
			max_found = pva->count;
			continue;
		}

		if (!_alloc_mirrored_area(lv, &allocated, pvm, pva, segtype,
					  mirrored_pv, mirrored_pe)) {
			stack;
			return 0;
		}

		break;
	}

	if (allocated != lv->le_count) {
		log_error("Insufficient contiguous allocatable extents (%u) "
			  "for logical volume %s: %u required",
			  allocated + max_found, lv->name, lv->le_count);
		return 0;
	}

	return 1;
}

/*
 * Areas just get allocated in order until the lv
 * is full.
 */
static int _alloc_next_free(struct logical_volume *lv,
			    struct list *pvms, uint32_t allocated)
{
	struct pv_map *pvm;
	struct pv_area *pva;

	list_iterate_items(pvm, pvms) {
		list_iterate_items(pva, &pvm->areas) {
			if (!_alloc_linear_area(lv, &allocated, pvm, pva) ||
			    (allocated == lv->le_count))
				goto done;
		}
	}

      done:
	if (allocated != lv->le_count) {
		log_error("Insufficient allocatable logical extents (%u) "
			  "for logical volume %s: %u required",
			  allocated, lv->name, lv->le_count);
		return 0;
	}

	return 1;
}

static int _alloc_virtual(struct logical_volume *lv,
			  uint32_t allocated, struct segment_type *segtype)
{
	struct lv_segment *seg;

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv, allocated,
				     lv->le_count - allocated, 0, 0, 0,
				     lv->le_count - allocated, 0, 0))) {
		log_error("Couldn't allocate new zero segment.");
		return 0;
	}

	list_add(&lv->segments, &seg->list);
	lv->status |= VIRTUAL;

	return 1;
}

struct lv_segment *alloc_snapshot_seg(struct logical_volume *lv,
				      uint32_t allocated)
{
	struct lv_segment *seg;
	struct segment_type *segtype;

	segtype = get_segtype_from_string(lv->vg->cmd, "snapshot");
	if (!segtype) {
		log_error("Failed to find snapshot segtype");
		return NULL;
	}

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, segtype, lv, allocated,
				     lv->le_count - allocated, 0, 0, 0,
				     lv->le_count - allocated, 0, 0))) {
		log_error("Couldn't allocate new snapshot segment.");
		return NULL;
	}

	list_add(&lv->segments, &seg->list);
	lv->status |= VIRTUAL;

	return seg;
}

/*
 * Chooses a correct allocation policy.
 */
static int _allocate(struct logical_volume *lv,
		     struct list *allocatable_pvs, uint32_t allocated,
		     alloc_policy_t alloc, struct segment_type *segtype,
		     uint32_t stripes, uint32_t stripe_size, uint32_t mirrors,
		     struct physical_volume *mirrored_pv, uint32_t mirrored_pe,
		     uint32_t status)
{
	int r = 0;
	struct pool *scratch;
	struct list *pvms, *old_tail = lv->segments.p, *segh;
	struct lv_segment *seg;

	if (segtype->flags & SEG_VIRTUAL)
		return _alloc_virtual(lv, allocated, segtype);

	if (!(scratch = pool_create("allocation", 1024))) {
		stack;
		return 0;
	}

	if (alloc == ALLOC_INHERIT)
		alloc = lv->vg->alloc;

	/*
	 * Build the sets of available areas on the pv's.
	 */
	if (!(pvms = create_pv_maps(scratch, lv->vg, allocatable_pvs)))
		goto out;

	if (stripes > 1 || mirrors > 1)
		r = _alloc_parallel(lv, pvms, allocated, stripes, stripe_size,
				    mirrors, segtype);

	else if (mirrored_pv)
		r = _alloc_mirrored(lv, pvms, allocated, segtype, mirrored_pv,
				    mirrored_pe);

	else if (alloc == ALLOC_CONTIGUOUS)
		r = _alloc_contiguous(lv, pvms, allocated);

	else if (alloc == ALLOC_NORMAL || alloc == ALLOC_ANYWHERE)
		r = _alloc_next_free(lv, pvms, allocated);

	else {
		log_error("Unrecognised allocation policy: "
			  "unable to set up logical volume.");
		goto out;
	}

	if (r) {
		lv->vg->free_count -= lv->le_count - allocated;

		/*
		 * Iterate through the new segments, updating pe
		 * counts in pv's.
		 */
		list_uniterate(segh, old_tail, &lv->segments) {
			seg = list_item(segh, struct lv_segment);
			_get_extents(seg);
			seg->status = status;
		}
	} else {
		/*
		 * Put the segment list back how we found it.
		 */
		old_tail->n = &lv->segments;
		lv->segments.p = old_tail;
	}

      out:
	pool_destroy(scratch);
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

/*
 * Entry point for all extent allocations
 */
int lv_extend(struct logical_volume *lv,
	      struct segment_type *segtype,
	      uint32_t stripes, uint32_t stripe_size,
	      uint32_t mirrors, uint32_t extents,
	      struct physical_volume *mirrored_pv, uint32_t mirrored_pe,
	      uint32_t status, struct list *allocatable_pvs,
	      alloc_policy_t alloc)
{
	uint32_t old_le_count = lv->le_count;
	uint64_t old_size = lv->size;

	lv->le_count += extents;
	lv->size += (uint64_t) extents *lv->vg->extent_size;

	if (lv->vg->fid->fmt->ops->segtype_supported &&
	    !lv->vg->fid->fmt->ops->segtype_supported(lv->vg->fid, segtype)) {
		log_error("Metadata format (%s) does not support required "
			  "LV segment type (%s).", lv->vg->fid->fmt->name,
			  segtype->name);
		log_error("Consider changing the metadata format by running "
			  "vgconvert.");
		return 0;
	}

	if (!_allocate(lv, allocatable_pvs, old_le_count, alloc,
		       segtype, stripes, stripe_size, mirrors, mirrored_pv,
		       mirrored_pe, status)) {
		lv->le_count = old_le_count;
		lv->size = old_size;
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

static int _lv_segment_reduce(struct lv_segment *seg, uint32_t reduction)
{
	_put_extents(seg);
	seg->len -= reduction;

	/* Caller must ensure exact divisibility */
	if (seg_is_striped(seg)) {
		if (reduction % seg->area_count) {
			log_error("Segment extent reduction %" PRIu32
				  "not divisible by #stripes %" PRIu32,
				  reduction, seg->area_count);
			return 0;
		}
		seg->area_len -= (reduction / seg->area_count);
	} else
		seg->area_len -= reduction;

	_shrink_lv_segment(seg);
	_get_extents(seg);

	return 1;
}

/*
 * Entry point for all extent reductions
 */
int lv_reduce(struct logical_volume *lv, uint32_t extents)
{
	struct list *segh;
	struct lv_segment *seg;
	uint32_t count = extents;
	uint32_t reduction;

	list_uniterate(segh, &lv->segments, &lv->segments) {
		seg = list_item(segh, struct lv_segment);

		if (!count)
			break;

		if (seg->len <= count) {
			/* remove this segment completely */
			list_del(segh);
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
	lv->vg->free_count += extents;

	if (lv->le_count && lv->vg->fid->fmt->ops->lv_setup &&
	    !lv->vg->fid->fmt->ops->lv_setup(lv->vg->fid, lv)) {
		stack;
		return 0;
	}

	return 1;
}

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

	return i;
}
