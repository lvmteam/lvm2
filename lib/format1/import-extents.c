/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "hash.h"
#include "dbg_malloc.h"
#include "log.h"
#include "pool.h"
#include "disk-rep.h"

/*
 * After much thought I have decided it is easier,
 * and probably no less efficient, to convert the
 * pe->le map to a full le->pe map, and then
 * process this to get the segments form that
 * we're after.  Any code which goes directly from
 * the pe->le map to segments would be gladly
 * accepted, if it is less complicated than this
 * file.
 */
struct pe_specifier {
	struct physical_volume *pv;
	uint32_t pe;
};

struct lv_map {
	struct logical_volume *lv;
	uint32_t stripes;
	uint32_t stripe_size;
	struct pe_specifier *map;
};

static struct hash_table *
_create_lv_maps(struct pool *mem, struct volume_group *vg)
{
	struct hash_table *maps = hash_create(32);
	struct list *llh;
	struct lv_list *ll;
	struct lv_map *lvm;

	if (!maps) {
		log_err("Unable to create hash table for holding "
			"extent maps.");
		return NULL;
	}

	list_iterate (llh, &vg->lvs) {
		ll = list_item(llh, struct lv_list);

		if (!(lvm = pool_alloc(mem, sizeof(*lvm)))) {
			stack;
			goto bad;
		}

		lvm->lv = &ll->lv;
		if (!(lvm->map = pool_zalloc(mem, sizeof(*lvm->map)
					     * ll->lv.le_count))) {
			stack;
			goto bad;
		}

		if (!hash_insert(maps, ll->lv.name, lvm)) {
			stack;
			goto bad;
		}
	}

	return maps;

 bad:
	hash_destroy(maps);
	return NULL;
}

static int _fill_lv_array(struct lv_map **lvs,
			  struct hash_table *maps, struct disk_list *dl)
{
	struct list *lvh;
	struct lv_map *lvm;

	memset(lvs, 0, sizeof(*lvs) * MAX_LV);
	list_iterate(lvh, &dl->lvds) {
		struct lvd_list *ll = list_item(lvh, struct lvd_list);

		if (!(lvm = hash_lookup(maps, strrchr(ll->lvd.lv_name, '/')
					      + 1 ))) {
			log_err("Physical volume (%s) contains an "
				"unknown logical volume (%s).",
				dev_name(dl->dev), ll->lvd.lv_name);
			return 0;
		}

		lvm->stripes = ll->lvd.lv_stripes;
		lvm->stripe_size = ll->lvd.lv_stripesize;

		lvs[ll->lvd.lv_number] = lvm;
	}

	return 1;
}

static int _fill_maps(struct hash_table *maps, struct volume_group *vg,
		      struct list *pvds)
{
	struct list *pvdh;
	struct disk_list *dl;
	struct physical_volume *pv;
	struct lv_map *lvms[MAX_LV], *lvm;
	struct pe_disk *e;
	uint32_t i, lv_num, le;

	list_iterate(pvdh, pvds) {
		dl = list_item(pvdh, struct disk_list);
		pv = find_pv(vg, dl->dev);
		e = dl->extents;

		/* build an array of lv's for this pv */
		if (!_fill_lv_array(lvms, maps, dl)) {
			stack;
			return 0;
		}

		for (i = 0; i < dl->pvd.pe_total; i++) {
			lv_num = e[i].lv_num;

			if (lv_num == UNMAPPED_EXTENT)
				continue;
			else {
				lv_num--;
				lvm = lvms[lv_num];

				if(!lvm) {
					log_err("invalid lv in extent map");
					return 0;
				}

				le = e[i].le_num;

				if (le >= lvm->lv->le_count) {
					log_err("logical extent number "
						"out of bounds");
					return 0;
				}

				if (lvm->map[le].pv) {
					log_err("logical extent (%u) "
						"already mapped.", le);
					return 0;
				}

				lvm->map[le].pv = pv;
				lvm->map[le].pe = i;
			}
		}
	}

	return 1;
}

static int _check_single_map(struct lv_map *lvm)
{
	uint32_t i;
	for (i = 0; i < lvm->lv->le_count; i++) {
		if (!lvm->map[i].pv) {
			log_err("Logical volume (%s) contains an incomplete "
				"mapping table.", lvm->lv->name);
			return 0;
		}
	}

	return 1;
}

static int _check_maps_are_complete(struct hash_table *maps)
{
	struct hash_node *n;
	struct lv_map *lvm;

	for (n = hash_get_first(maps); n; n = hash_get_next(maps, n)) {
		lvm = (struct lv_map *) hash_get_data(maps, n);

		if (!_check_single_map(lvm)) {
			stack;
			return 0;
		}
	}
	return 1;
}

static int _same_segment(struct stripe_segment *seg, struct lv_map *lvm,
			 uint32_t count)
{
	uint32_t s;
	uint32_t le = seg->le + (count * seg->stripes);

	for (s = 0; s < seg->stripes; s++) {
		if ((lvm->map[le + s].pv != seg->area[s].pv) ||
		    (lvm->map[le + s].pe != seg->area[s].pe + count))
			return 0;
	}
	return 1;
}

static int _build_segments(struct pool *mem, struct lv_map *lvm)
{
	uint32_t stripes = lvm->stripes;
	uint32_t le, s, count;
	struct stripe_segment *seg;
	size_t len;

	len = sizeof(*seg) * (stripes * sizeof(seg->area[0]));

	le = 0;
	while (le < lvm->lv->le_count) {
		if (!(seg = pool_zalloc(mem, len))) {
			stack;
			return 0;
		}

		seg->lv = lvm->lv;
		seg->le = le;
		seg->len = 0;
		seg->stripe_size = lvm->stripe_size;
		seg->stripes = stripes;

		for (s = 0; s < stripes; s++) {
			seg->area[s].pv = lvm->map[le + s].pv;
			seg->area[s].pe = lvm->map[le + s].pe;
		}

		count = 1;
		do {
			le += stripes;
			seg->len += stripes;

		} while (_same_segment(seg, lvm, count++));

		list_add(&lvm->lv->segments, &seg->list);
	}

	return 1;
}

static int _build_all_segments(struct pool *mem, struct hash_table *maps)
{
	struct hash_node *n;
	struct lv_map *lvm;

	for (n = hash_get_first(maps); n; n = hash_get_next(maps, n)) {
		lvm = (struct lv_map *) hash_get_data(maps, n);
		if (!_build_segments(mem, lvm)) {
			stack;
			return 0;
		}
	}

	return 1;
}

int import_extents(struct pool *mem, struct volume_group *vg,
		   struct list *pvds)
{
	int r = 0;
	struct pool *scratch = pool_create(10 * 1024);
	struct hash_table *maps;

	if (!scratch) {
		stack;
		return 0;
	}

	if (!(maps = _create_lv_maps(scratch, vg))) {
		log_err("Couldn't allocate logical volume maps.");
		goto out;
	}

	if (!_fill_maps(maps, vg, pvds)) {
		log_err("Couldn't fill logical volume maps.");
		goto out;
	}

	if (!_check_maps_are_complete(maps)) {
		stack;
		goto out;
	}

	if (!_build_all_segments(mem, maps)) {
		log_err("Couldn't build extent segments.");
		goto out;
	}
	r = 1;

 out:
	if (maps)
		hash_destroy(maps);
	pool_destroy(scratch);
	return r;
}
