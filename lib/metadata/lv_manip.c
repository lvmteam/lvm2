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
#include "display.h"
#include "segtypes.h"

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

		pv = seg->area[s].u.pv.pv;
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

		pv = seg->area[s].u.pv.pv;

		if (pv) {
			count = seg->area_len;
			assert(pv->pe_alloc_count >= count);
			pv->pe_alloc_count -= count;
		}
	}
}

struct lv_segment *alloc_lv_segment(struct pool *mem, uint32_t num_areas)
{
	struct lv_segment *seg;
	uint32_t len = sizeof(*seg) + (num_areas * sizeof(seg->area[0]));

	if (!(seg = pool_zalloc(mem, len))) {
		stack;
		return NULL;
	}

	seg->area_count = num_areas;
	list_init(&seg->tags);

	return seg;
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
	if (segtype->flags & SEG_AREAS_STRIPED)
		striped = 1;

	count = lv->le_count - *ix;
	area_len = count / (striped ? area_count : 1);
	smallest = areas[area_count - 1]->count;

	if (smallest < area_len)
		area_len = smallest;

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, area_count))) {
		log_err("Couldn't allocate new parallel segment.");
		return 0;
	}

	seg->lv = lv;
	seg->segtype = segtype;
	seg->le = *ix;
	seg->len = area_len * (striped ? area_count : 1);
	seg->area_len = area_len;
	seg->stripe_size = stripe_size;
	seg->extents_copied = 0u;

	for (s = 0; s < area_count; s++) {
		struct pv_area *pva = areas[s];
		seg->area[s].type = AREA_PV;
		seg->area[s].u.pv.pv = pva->map->pvl->pv;
		seg->area[s].u.pv.pe = pva->start;
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
	struct list *pvmh;
	struct pv_area **areas;
	unsigned int pv_count = 0, ix;
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

	list_iterate(pvmh, pvms)
	    pv_count++;

	/* allocate an array of pv_areas, one candidate per pv */
	len = sizeof(*areas) * pv_count;
	if (!(areas = dbg_malloc(sizeof(*areas) * pv_count))) {
		log_err("Couldn't allocate areas array.");
		return 0;
	}

	while (allocated != lv->le_count) {

		ix = 0;
		list_iterate(pvmh, pvms) {
			pvm = list_item(pvmh, struct pv_map);

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

	count = pva->count;
	remaining = lv->le_count - *ix;
	if (count > remaining)
		count = remaining;

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, 1))) {
		log_err("Couldn't allocate new stripe segment.");
		return 0;
	}

	seg->lv = lv;
	if (!(seg->segtype = get_segtype_from_string(lv->vg->cmd, "striped"))) {
		stack;
		return 0;
	}
	seg->le = *ix;
	seg->len = count;
	seg->area_len = count;
	seg->stripe_size = 0;
	seg->area[0].type = AREA_PV;
	seg->area[0].u.pv.pv = map->pvl->pv;
	seg->area[0].u.pv.pe = pva->start;

	list_add(&lv->segments, &seg->list);

	consume_pv_area(pva, count);
	*ix += count;
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

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, 2))) {
		log_err("Couldn't allocate new mirrored segment.");
		return 0;
	}

	seg->lv = lv;
	seg->segtype = segtype;
	seg->le = *ix;
	seg->status = 0u;
	seg->len = count;
	seg->area_len = count;
	seg->stripe_size = 0;
	seg->extents_copied = 0u;
	/* FIXME Remove AREA_PV restriction here? */
	seg->area[0].type = AREA_PV;
	seg->area[0].u.pv.pv = mirrored_pv;
	seg->area[0].u.pv.pe = mirrored_pe;
	seg->area[1].type = AREA_PV;
	seg->area[1].u.pv.pv = map->pvl->pv;
	seg->area[1].u.pv.pe = pva->start;
	list_add(&lv->segments, &seg->list);

	consume_pv_area(pva, count);
	*ix += count;
	return 1;
}

/*
 * Only one area per pv is allowed, so we search
 * for the biggest area, or the first area that
 * can complete the allocation.
 */

/*
 * FIXME: subsequent lvextends may not be contiguous.
 */
static int _alloc_contiguous(struct logical_volume *lv,
			     struct list *pvms, uint32_t allocated)
{
	struct list *tmp1;
	struct pv_map *pvm;
	struct pv_area *pva;

	list_iterate(tmp1, pvms) {
		pvm = list_item(tmp1, struct pv_map);

		if (list_empty(&pvm->areas))
			continue;

		/* first item in the list is the biggest */
		pva = list_item(pvm->areas.n, struct pv_area);
		if (pva->count < lv->le_count)
			continue;

		if (!_alloc_linear_area(lv, &allocated, pvm, pva)) {
			stack;
			return 0;
		}

		break;
	}

	if (allocated != lv->le_count) {
		log_error("Insufficient allocatable extents (%u) "
			  "for logical volume %s: %u required",
			  allocated, lv->name, lv->le_count);
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
	struct list *tmp1;
	struct pv_map *pvm;
	struct pv_area *pva;
	uint32_t max_found = 0;

	/* Try each PV in turn */
	list_iterate(tmp1, pvms) {
		pvm = list_item(tmp1, struct pv_map);

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
	struct list *tmp1, *tmp2;
	struct pv_map *pvm;
	struct pv_area *pva;

	list_iterate(tmp1, pvms) {
		pvm = list_item(tmp1, struct pv_map);

		list_iterate(tmp2, &pvm->areas) {
			pva = list_item(tmp2, struct pv_area);
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

	if (!(seg = alloc_lv_segment(lv->vg->cmd->mem, 0))) {
		log_err("Couldn't allocate new zero segment.");
		return 0;
	}

	seg->lv = lv;
	seg->segtype = segtype;
	seg->status = 0u;
	seg->le = allocated;
	seg->len = lv->le_count - allocated;
	seg->area_len = seg->len;
	seg->stripe_size = 0;
	seg->extents_copied = 0u;
	list_add(&lv->segments, &seg->list);
	lv->status |= VIRTUAL;

	return 1;
}

/*
 * Chooses a correct allocation policy.
 */
static int _allocate(struct volume_group *vg, struct logical_volume *lv,
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

	if (!(scratch = pool_create(1024))) {
		stack;
		return 0;
	}

	if (alloc == ALLOC_INHERIT)
		alloc = vg->alloc;

	/*
	 * Build the sets of available areas on the pv's.
	 */
	if (!(pvms = create_pv_maps(scratch, vg, allocatable_pvs)))
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
		vg->free_count -= lv->le_count - allocated;

		/*
		 * Iterate through the new segments, updating pe
		 * counts in pv's.
		 */
		for (segh = lv->segments.p; segh != old_tail; segh = segh->p) {
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
	struct list *lvh;
	struct logical_volume *lv;
	int high = -1, i;

	list_iterate(lvh, &vg->lvs) {
		lv = (list_item(lvh, struct lv_list)->lv);

		if (sscanf(lv->name, format, &i) != 1)
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
				       uint32_t status,
				       alloc_policy_t alloc,
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
	list_init(&lv->segments);
	list_init(&lv->tags);

	if (fi->fmt->ops->lv_setup && !fi->fmt->ops->lv_setup(fi, lv)) {
		stack;
		if (ll)
			pool_free(cmd->mem, ll);
		return NULL;
	}

	vg->lv_count++;
	list_add(&vg->lvs, &ll->list);

	return lv;
}

int lv_extend(struct format_instance *fid,
	      struct logical_volume *lv,
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

	if (!_allocate(lv->vg, lv, allocatable_pvs, old_le_count, alloc,
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

	if (fid->fmt->ops->lv_setup && !fid->fmt->ops->lv_setup(fid, lv)) {
		stack;
		return 0;
	}

	return 1;
}

int lv_reduce(struct format_instance *fi,
	      struct logical_volume *lv, uint32_t extents)
{
	struct list *segh;
	struct lv_segment *seg;
	uint32_t count = extents;
	int striped;

	for (segh = lv->segments.p;
	     (segh != &lv->segments) && count; segh = segh->p) {
		seg = list_item(segh, struct lv_segment);

		if (seg->len <= count) {
			/* remove this segment completely */
			count -= seg->len;
			_put_extents(seg);
			list_del(segh);
		} else {
			/* reduce this segment */
			_put_extents(seg);
			seg->len -= count;
			striped = seg->segtype->flags & SEG_AREAS_STRIPED;
			/* Caller must ensure exact divisibility */
			if (striped && (count % seg->area_count)) {
				log_error("Segment extent reduction %" PRIu32
					  "not divisible by #stripes %" PRIu32,
					  count, seg->area_count);
				return 0;
			}
			seg->area_len -=
			    count / (striped ? seg->area_count : 1);
			_get_extents(seg);
			count = 0;
		}
	}

	lv->le_count -= extents;
	lv->size = (uint64_t) lv->le_count * lv->vg->extent_size;
	lv->vg->free_count += extents;

	if (fi->fmt->ops->lv_setup && !fi->fmt->ops->lv_setup(fi, lv)) {
		stack;
		return 0;
	}

	return 1;
}

int lv_remove(struct volume_group *vg, struct logical_volume *lv)
{
	struct list *segh;
	struct lv_list *lvl;

	/* find the lv list */
	if (!(lvl = find_lv_in_vg(vg, lv->name))) {
		stack;
		return 0;
	}

	/* iterate through the lv's segments freeing off the pe's */
	list_iterate(segh, &lv->segments)
	    _put_extents(list_item(segh, struct lv_segment));

	vg->lv_count--;
	vg->free_count += lv->le_count;

	list_del(&lvl->list);

	return 1;
}

uint32_t find_free_lvnum(struct logical_volume *lv)
{
	int lvnum_used[MAX_RESTRICTED_LVS + 1];
	uint32_t i = 0;
	struct list *lvh;
	struct lv_list *lvl;
	int lvnum;

	memset(&lvnum_used, 0, sizeof(lvnum_used));

	list_iterate(lvh, &lv->vg->lvs) {
		lvl = list_item(lvh, struct lv_list);
		lvnum = lvnum_from_lvid(&lvl->lv->lvid);
		if (lvnum <= MAX_RESTRICTED_LVS)
			lvnum_used[lvnum] = 1;
	}

	while (lvnum_used[i])
		i++;

	return i;
}
