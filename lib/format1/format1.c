/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "disk-rep.h"
#include "dbg_malloc.h"
#include "pool.h"
#include "hash.h"
#include "list.h"


struct v1 {
	struct pool *mem;
	struct dev_filter *filter;
};

static int _import_vg(struct volume_group *vg, struct list_head *pvs)
{
	struct list_head *tmp;
	struct disk_list *dl;
	struct vg_disk *first = NULL;

	/* check all the vg's are the same */
	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);

		if (!first) {
			first = &dl->vg;

			memcpy(vg->id, &first->vg_uuid, ID_LEN);
			vg->name = NULL;
			vg->status = first->vg_status;
			vg->access = first->vg_access;
			vg->extent_size = first->pe_size;
			vg->extent_count = first->pe_total;
			vg->free_count = first->pe_total - first->pe_allocated;
			vg->max_lv = first->lv_max;
			vg->max_pv = first->pv_max;

		} else if (memcmp(first, &dl->vg, sizeof(*first))) {
			log_err("vg data differs on pvs\n");
			return 0;
		}
	}

	return 1;
}

static int _import_pvs(struct pool *mem, struct volume_group *vg,
		       struct list_head *pvs)
{
	struct list_head *tmp;
	struct disk_list *dl;
	struct pv_list *pvl;
	struct physical_volume *pv;

	vg->pv_count = 0;
	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);
		pvl = pool_alloc(mem, sizeof(*pvl));

		if (!pvl) {
			stack;
			return 0;
		}

		pv = &pvl->pv;
		memcpy(&pv->id, &dl->pv.pv_uuid, ID_LEN);
		pv->dev = ??;
		pv->vg_name = pool_strdup(dl->pv.vg_name);

		if (!pv->vg_name) {
			stack;
			return 0;
		}

		pv->exported = ??;
		pv->status = dl->pv.pv_status;
		pv->size = dl->pv.pv_size;
		pv->pe_size = dl->pv.pv_size;
		pe_start = dl->pv.pe_start;
		pe_count = dl->pv.pe_count;
		pe_allocated = dl->pv.pe_allocated;

		list_add(&pvl->list, vg->pvs);
		vg->pv_count++;
	}

	return 1;
}

static struct logical_volume *_find_lv(struct volume_group *vg,
				       const char *name)
{
	struct list_head *tmp;
	struct logical_volume *lv;

	list_for_each(tmp, &vg->lvs) {
		lv = list_entry(tmp, struct logical_volume, list);
		if (!strcmp(name, lv->name))
			return lv;
	}
	return NULL;
}

static struct logical_volume *_add_lv(struct volume_group *vg,
				      struct lv_disk *lvd)
{
	struct logical_volume *lv = pool_alloc(mem, sizeof(*lv));

	if (!lv) {
		stack;
		return 0;
	}

	memset(lv->id, 0, sizeof(lv->id));
        if (!(lv->name = pool_dupstr(lvd->lv_name))) {
		stack;
		return 0;
	}

        lv->access = lvd->lv_access;
        lv->status = lvd->lv_status;
        lv->open = lvd->lv_open;
        lv->size = lvd->lv_size;
        lv->le_count = lvd->lv_allocated_lv;
	lv->map = pool_alloc(mem, sizeof(struct pe_specifier) * lv->le_count);

	if (!lv->map) {
		stack;
		return 0;
	}

	return 1;
}

static int _import_lvs(struct pool *mem, struct volume_group *vg,
		       struct list_head *pvs)
{
	struct list_head *tmp, tmp2;
	struct disk_list *dl;
	struct lv_list *ll;
	struct lv_disk *lvd;
	struct logical_volume *lv;
	int i;

	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);
		list_for_each(tmp2, &dl->lvs) {
			ll = list_entry(tmp2, struct lv_list, list);
			lvd = &ll->lv;

			if (!_find_lv(vg, lvd->lvname) && !_add_lv(vg, lvd)) {
				stack;
				return 0;
			}
		}
	}

	return 1;
}

static int _fill_lv_array(struct logical_volume *lvs,
			  struct volume_group *vg, struct disk_list *dl)
{
	struct list_head *tmp;
	struct pv_disk *pvd = &dl->pv;
	struct logical_volume *lv;
	int i = 0;

	list_for_each(tmp, &dl->lvs) {
		struct lv_list *ll = list_entry(tmp, struct lv_disk, list);

		if (!(lv = _find_lv(vg, ll->lv.name))) {
			stack;
			return 0;
		}

		lvs[i] = lv;
		i++;
	}

	return 1;
}

static int _import_extents(struct pool *mem, struct volume_group *vg,
			   struct list_head *pvs)
{
	struct list_head *tmp;
	struct disk_list *dl;
	struct logical_volume *lv, lvs[MAX_LV];
	struct physical_volume *pv;
	struct pe_disk *e;
	int i;
	uint32_t lv_num, le;

	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);
		pv = _find_pv(vg, dl->pv.pv_name);
		e = dl->extents;

		/* build an array of lv's for this pv */
		if (!_fill_lv_array(lvs, vg, dl)) {
			stack;
			return 0;
		}

		for (i = 0; i < dl->pv.pe_total; i++) {
			lv_num = e[i].lv_num;

			if (lv_num == UNMAPPED_EXTENT)
				lv->map[le].pv = NULL;

			else if(lv_num >= dl->pv.lv_cur) {
				log_err("invalid lv in extent map\n");
				return 0;

			else {
				lv = lvs[lv_num];
				le = e[i].le_num;

				lv->map[le].pv = pv;
				lv->map[le].pe = i;
			}
		}
	}

	return 1;
}

static struct volume_group _build_vg(struct pool *mem, struct list_head *pvs)
{
	struct volume_group *vg = pool_alloc(mem, sizeof(*vg));

	if (!vg) {
		stack;
		return 0;
	}

	memset(vg, 0, sizeof(*vg));

	if (!_import_vg(vg, pvs))
		goto bad;

	if (!_import_pvs(mem, vg, pvs))
		goto bad;

	if (!_import_lvs(mem, vg, pvs))
		goto bad;

	return vg;

 bad:
	stack;
	pool_free(mem, vg);
	return NULL;
}

static struct volume_group _vg_read(struct io_space *is, const char *vg_name)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list_head pvs;
	struct volume_group *vg;

	if (!mem) {
		stack;
		return NULL;
	}

	if (!read_pvs_in_vg(vg_name, is->filter, mem, &pvs)) {
		stack;
		return NULL;
	}

	if (!(vg = _build_vg(is->mem, &pvs))) {
		stack;
	}

	pool_destroy(mem);
	return vg;
}

static struct disk_list _flatten_pv(struct pool *mem, struct volume_group *vg, 
				    struct physical_volume *pv)
{
	
}

static int _flatten_vg(struct pool *mem, struct volume_group *vg,
		       struct list_head *pvs)
{
	struct list_head *tmp;
	struct physical_volume *pv;
	struct disk_list *data;

	list_for_each(tmp, &vg->pvs) {
		pv = list_entry(tmp, struct physical_volume, list);

		if (!(data = _flatten_pv(vg, pv))) {
			stack;
			return 0;
		}

		list_add(&data->list, pvs);
	}
	return 1;
}

static int _vg_write(struct io_space *is, struct volume_group *vg)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list_head pvs;
	int r = 0;

	if (!mem) {
		stack;
		return 0;
	}

	r = _flatten_vg(mem, vg, &pvs) && write_pvs(&pvs);
	pool_destroy(mem);
	return r;
}

struct io_space *create_lvm1_format(struct device_manager *mgr)
{

}


