/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "pv_map.h"
#include "log.h"

/*
 * The heart of the allocation code.  This
 * function takes a pv_area and allocates it to
 * the lv.  If the lv doesn't need the complete
 * area then the area is split, otherwise the area
 * is unlinked from the pv_map.
 */
static int _alloc_area(struct logical_volume *lv, uint32_t index,
		       struct physical_volume *pv, struct pv_area *pva)
{
	uint32_t count, remaining, i, start;

	start = pva->start;

	count = pva->count;
	remaining = lv->le_count - index;

	if (remaining < count) {
		/* split the area */
		count = remaining;
		pva->start += count;
		pva->count -= count;

	} else {
		/* unlink the area */
		list_del(&pva->list);
	}

	for (i = 0; i < count; i++) {
		lv->map[i + index].pv = pv;
		lv->map[i + index].pe = start + i;
	}

	return count;
}

static int _alloc_striped(struct logical_volume *lv,
			  struct list *pvms, uint32_t allocated)
{
	/* FIXME: finish */
	log_error("Striped allocation not implemented yet in LVM2.");
	return 0;
}

/*
 * Only one area per pv is allowed, so we search
 * for the biggest area, or the first area that
 * can complete the allocation.
 */
static int _alloc_contiguous(struct logical_volume *lv,
			     struct list *pvms, uint32_t allocated)
{
	struct list *tmp1, *tmp2;
	struct pv_map *pvm;
	struct pv_area *pva, *biggest;

	list_iterate (tmp1, pvms) {
		pvm = list_item(tmp1, struct pv_map);
		biggest = NULL;

		list_iterate (tmp2, &pvm->areas) {
			pva = list_item(tmp2, struct pv_area);

			if (!biggest || (pva->count > biggest->count))
				biggest = pva;

			if (biggest->count >= (lv->le_count - allocated))
				break;
		}

		allocated += _alloc_area(lv, allocated, pvm->pv, pva);
		if (allocated == lv->le_count)
			break;
	}

	if (allocated != lv->le_count) {
		log_error("Insufficient free extents to "
			  "allocate logical volume %s: %u required", 
			  lv->name, lv->le_count);
		return 0;
	}

	return 1;
}

/*
 * Areas just get allocated in order until the lv
 * is full.
 */
static int _alloc_simple(struct logical_volume *lv,
			 struct list *pvms, uint32_t allocated)
{
	struct list *tmp1, *tmp2;
	struct pv_map *pvm;
	struct pv_area *pva;

	list_iterate (tmp1, pvms) {
		pvm = list_item(tmp1, struct pv_map);

		list_iterate (tmp2, &pvm->areas) {
			pva = list_item(tmp2, struct pv_area);
			allocated += _alloc_area(lv, allocated, pvm->pv, pva);

			if (allocated == lv->le_count)
				break;
		}

		if (allocated == lv->le_count) /* FIXME: yuck, repeated test */
			break;
	}

	if (allocated != lv->le_count) {
		log_error("Insufficient free logical extents to "
			  "allocate logical volume %s: %u required", 
			  lv->name, lv->le_count);
		return 0;
	}

	return 1;
}

/*
 * Chooses a correct allocation policy.
 */
static int _allocate(struct volume_group *vg, struct logical_volume *lv,
		     struct list *acceptable_pvs, uint32_t allocated)
{
	int r = 0;
	struct pool *scratch;
	struct list *pvms;

	if (!(scratch = pool_create(1024))) {
		stack;
		return 0;
	}

	/*
	 * Build the sets of available areas on
	 * the pv's.
	 */
	if (!(pvms = create_pv_maps(scratch, vg, acceptable_pvs))) {
		goto out;
	}

	if (lv->stripes > 1)
		r = _alloc_striped(lv, pvms, allocated);

	else if (lv->status & ALLOC_CONTIGUOUS)
		r = _alloc_contiguous(lv, pvms, allocated);

	else
		r = _alloc_simple(lv, pvms, allocated);

	if (r) {
		vg->lv_count++;
		vg->free_count -= lv->le_count - allocated;
	}

 out:
	pool_destroy(scratch);
	return r;
}

struct logical_volume *lv_create(struct io_space *ios,
				 const char *name,
				 uint32_t status,
				 uint32_t stripes,
				 uint32_t stripe_size,
				 uint32_t extents,
				 struct volume_group *vg,
				 struct list *acceptable_pvs)
{
	struct lv_list *ll = NULL;
	struct logical_volume *lv;

	if (!extents) {
		log_error("Unable to create logical volume %s with no extents",
			  name);
		return NULL;
	}

	if (vg->free_count < extents) {
		log_error("Insufficient free extents (%u) in volume group %s: "
			  "%u required", vg->free_count, vg->name, extents);
		return NULL;
	}

	if (vg->max_lv == vg->lv_count) {
		log_error("Maximum number of logical volumes (%u) reached "
			  "in volume group %s", vg->max_lv, vg->name);
		return NULL;
	}

	if (!(ll = pool_zalloc(ios->mem, sizeof(*ll)))) {
		stack;
		return NULL;
	}

	list_init(&ll->list);

	lv = &ll->lv;

	strcpy(lv->id.uuid, "");

	if (!(lv->name = pool_strdup(ios->mem, name))) {
		stack;
		goto bad;
	}

	lv->vg = vg;
	lv->status = status;
	lv->read_ahead = 0;
	lv->stripes = stripes;
	lv->size = extents * vg->extent_size;
	lv->le_count = extents;

	if (!(lv->map = pool_zalloc(ios->mem, sizeof(*lv->map) * extents))) {
		stack;
		goto bad;
	}

	if (!_allocate(vg, lv, acceptable_pvs, 0u)) {
		stack;
		goto bad;
	}

	list_add(&vg->lvs, &ll->list);
	return lv;

 bad:
	if (ll)
		pool_free(ios->mem, ll);

	return NULL;
}

int lv_reduce(struct io_space *ios,
	      struct logical_volume *lv, uint32_t extents)
{
	if (extents % lv->stripes) {
		log_error("For a striped volume you must reduce by a "
			  "multiple of the number of stripes");
		return 0;
	}

	if (lv->le_count <= extents) {
		log_error("Attempt to reduce by so many extents there would "
			  "be nothing left of the logical volume.");
		return 0;
	}

	/* Hmmm ... I think all we need to do is ... */
	lv->le_count -= extents;
	return 1;
}

int lv_extend(struct io_space *ios,
	      struct logical_volume *lv, uint32_t extents,
	      struct list *acceptable_pvs)
{
	struct pe_specifier *new_map;
	struct logical_volume *new_lv;

	if (!(new_map = pool_zalloc(ios->mem, sizeof(*new_map) * 
				    (extents + lv->le_count)))) {
		stack;
		return 0;
	}

	memcpy(new_map, lv->map, sizeof(*new_map) * lv->le_count);

	if (!(new_lv = pool_alloc(ios->mem, sizeof(*new_lv)))) {
		pool_free(ios->mem, new_map);
		stack;
		return 0;
	}
	
	memcpy(new_lv, lv, sizeof(*lv));
	new_lv->map = new_map;
	new_lv->le_count += extents;

	if (!_allocate(lv->vg, lv, acceptable_pvs, lv->le_count)) {
		stack;
		goto bad;
	}

	memcpy(lv, new_lv, sizeof(*lv));

	/* now you see why new_lv had to be allocated last */
	pool_free(ios->mem, new_lv);
	return 1;

 bad:
	pool_free(ios->mem, new_map);
	return 0;
}
