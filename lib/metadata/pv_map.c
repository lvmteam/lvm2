/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "pv_map.h"
#include "log.h"
#include "hash.h"

#include <assert.h>

static int _create_maps(struct pool *mem, struct list *pvs, struct list *maps)
{
	struct list *tmp;
	struct physical_volume *pv;
	struct pv_map *pvm;

	list_iterate(tmp, pvs) {
		pv = list_item(tmp, struct pv_list)->pv;

		if (!(pv->status & ALLOCATABLE_PV))
			continue;

		if (!(pvm = pool_zalloc(mem, sizeof(*pvm)))) {
			stack;
			return 0;
		}

		pvm->pv = pv;
		if (!(pvm->allocated_extents =
		      bitset_create(mem, pv->pe_count))) {
			stack;
			return 0;
		}

		list_init(&pvm->areas);
		list_add(maps, &pvm->list);
	}

	return 1;
}

static int _set_allocated(struct hash_table *hash,
			  struct physical_volume *pv, int pe)
{
	struct pv_map *pvm;

	if (!(pvm = (struct pv_map *) hash_lookup(hash, dev_name(pv->dev)))) {
		/*
		 * it doesn't matter that this fails, it just
		 * means this part of the lv is on a pv that
		 * we're not interested in allocating to.
		 */
		return 1;
	}

	/* sanity check */
	if (bit(pvm->allocated_extents, pe)) {
		log_error("Physical extent %d of %s referenced by more than "
			  "one logical volume", pe, dev_name(pv->dev));
		return 0;
	}

	bit_set(pvm->allocated_extents, pe);
	return 1;
}

static int _fill_bitsets(struct volume_group *vg, struct list *maps)
{
	struct list *lvh, *pvmh, *segh;
	struct logical_volume *lv;
	struct pv_map *pvm;
	uint32_t s, pe;
	struct hash_table *hash;
	struct stripe_segment *seg;
	int r = 0;

	if (!(hash = hash_create(128))) {
		log_err("Couldn't create hash table for pv maps.");
		return 0;
	}

	/* populate the hash table */
	list_iterate (pvmh, maps) {
		pvm = list_item(pvmh, struct pv_map);
		if (!hash_insert(hash, dev_name(pvm->pv->dev), pvm)) {
			stack;
			goto out;
		}
	}

	/* iterate through all the lv's setting bit's for used pe's */
	list_iterate (lvh, &vg->lvs) {
		lv = &(list_item(lvh, struct lv_list)->lv);

		list_iterate (segh, &lv->segments) {
			seg = list_item(segh, struct stripe_segment);

			for (s = 0; s < seg->stripes; s++) {
				for (pe = 0; pe < (seg->len / seg->stripes);
				     pe++) {
					if (!_set_allocated(hash,
					    		    seg->area[s].pv,
					    		    seg->area[s].pe
							    + pe)) {
						stack;
						goto out;
					}
				}
			}
		}
	}
	r = 1;

 out:
	hash_destroy(hash);
	return r;
}

/*
 * Areas are maintained in size order.
 */
static void _insert_area(struct list *head, struct pv_area *a)
{
	struct list *pvah;
	struct pv_area *pva;

	if (list_empty(head)) {
		list_add(head, &a->list);
		return;
	}

	list_iterate (pvah, head) {
		pva = list_item(pvah, struct pv_area);

		if (pva->count < a->count)
			break;
	}

	list_add_h(&pva->list, &a->list);
}

static int _create_single_area(struct pool *mem, struct pv_map *pvm,
			       uint32_t *extent)
{
	uint32_t e = *extent, b, count = pvm->pv->pe_count;
	struct pv_area *pva;

	while (e < count && bit(pvm->allocated_extents, e))
		e++;

	if (e == count) {
		*extent = e;
		return 1;
	}

	b = e++;

	while (e < count && !bit(pvm->allocated_extents, e))
		e++;

	if (!(pva = pool_zalloc(mem, sizeof(*pva)))) {
		stack;
		return 0;
	}

	pva->map = pvm;
	pva->start = b;
	pva->count = e - b;
	_insert_area(&pvm->areas, pva);
	*extent = e;

	return 1;
}

static int _create_areas(struct pool *mem, struct pv_map *pvm)
{
	uint32_t pe = 0;

	while (pe < pvm->pv->pe_count)
		if (!_create_single_area(mem, pvm, &pe)) {
			stack;
			return 0;
		}

	return 1;
}

static int _create_all_areas(struct pool *mem, struct list *maps)
{
	struct list *tmp;
	struct pv_map *pvm;

	list_iterate(tmp, maps) {
		pvm = list_item(tmp, struct pv_map);

		if (!_create_areas(mem, pvm)) {
			stack;
			return 0;
		}
	}

	return 1;
}

struct list *create_pv_maps(struct pool *mem, struct volume_group *vg,
			    struct list *pvs)
{
	struct list *maps = pool_zalloc(mem, sizeof(*maps));

	if (!maps) {
		stack;
		return NULL;
	}

	list_init(maps);

	if (!_create_maps(mem, pvs, maps)) {
		log_error("Couldn't create physical volume maps in %s",
			  vg->name);
		goto bad;
	}

	if (!_fill_bitsets(vg, maps)) {
		log_error("Couldn't fill extent allocation bitmaps in %s",
			  vg->name);
		goto bad;
	}

	if (!_create_all_areas(mem, maps)) {
		log_error("Couldn't create area maps in %s", vg->name);
		goto bad;
	}

	return maps;

      bad:
	pool_free(mem, maps);
	return NULL;
}

void consume_pv_area(struct pv_area *pva, uint32_t to_go)
{
	list_del(&pva->list);

	assert(to_go <= pva->count);

	if (to_go < pva->count) {
		/* split the area */
		pva->start += to_go;
		pva->count -= to_go;
		_insert_area(&pva->map->areas, pva);
	}
}
