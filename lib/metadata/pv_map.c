/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "pv_map.h"
#include "log.h"

static int _create_maps(struct pool *mem, struct list *pvs, struct list *maps)
{
	struct list *tmp;
	struct physical_volume *pv;
	struct pv_map *pvm;

	list_iterate(tmp, pvs) {
		pv = &(list_item(tmp, struct pv_list)->pv);

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

static int _fill_bitsets(struct volume_group *vg, struct list *maps)
{
	/*
	 * FIXME: should put pvm's in a table for
	 * O(1) access, and remove the nasty inner
	 * loop in this code.
	 */
	struct list *lvh, *pvmh;
	struct logical_volume *lv;
	struct pe_specifier *pes;
	struct pv_map *pvm;
	uint32_t le;

	list_iterate(lvh, &vg->lvs) {
		lv = &(list_item(lvh, struct lv_list)->lv);

		for (le = 0; le < lv->le_count; le++) {
			pes = lv->map + le;

			/* this is the nasty that will kill performance */
			list_iterate(pvmh, maps) {
				pvm = list_item(pvmh, struct pv_map);

				if (pvm->pv == pes->pv)
					break;
			}

			/* not all pvs are necc. in the list */
			if (pvmh == maps)
				continue;

			bit_set(pvm->allocated_extents, pes->pe);
		}
	}

	return 1;
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

	pva->start = b;
	pva->count = e - b;
	list_add(&pvm->areas, &pva->list);
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
