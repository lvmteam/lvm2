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
#include "log.h"


static int _import_vg(struct pool *mem,
		      struct volume_group *vg, struct list_head *pvs)
{
	struct list_head *tmp;
	struct disk_list *dl;
	struct vg_disk *first = NULL;

	/* check all the vg's are the same */
	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);

		if (!first) {
			first = &dl->vg;

			memcpy(&vg->id.uuid, &first->vg_uuid, ID_LEN);
			if (!(vg->name = pool_strdup(mem, dl->pv.vg_name))) {
				stack;
				return 0;
			}

			// FIXME: encode flags
			//vg->status = first->vg_status;
			//vg->access = first->vg_access;
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

	return first ? 1 : 0;
}

static int _import_pv(struct pool *mem,
		      struct disk_list *dl, struct physical_volume *pv)
{
	memset(pv, 0, sizeof(*pv));
	memcpy(&pv->id, &dl->pv.pv_uuid, ID_LEN);

	pv->dev = dl->dev;
	pv->vg_name = pool_strdup(mem, dl->pv.vg_name);

	if (!pv->vg_name) {
		stack;
		return 0;
	}

	// FIXME: finish
	//pv->exported = ??;
	pv->status = dl->pv.pv_status;
	pv->size = dl->pv.pv_size;
	pv->pe_size = dl->pv.pv_size;
	pv->pe_start = dl->pv.pe_start;
	pv->pe_count = dl->pv.pe_total;
	pv->pe_allocated = dl->pv.pe_allocated;
	return 1;
}

static int _import_pvs(struct pool *mem, struct list_head *pvs,
		       struct list_head *results, int *count)
{
	struct list_head *tmp;
	struct disk_list *dl;
	struct pv_list *pvl;

	*count = 0;
	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);
		pvl = pool_alloc(mem, sizeof(*pvl));

		if (!pvl) {
			stack;
			return 0;
		}

		if (!_import_pv(mem, dl, &pvl->pv)) {
			stack;
			return 0;
		}

		list_add(&pvl->list, results);
		(*count)++;
	}

	return 1;
}

static struct logical_volume *_find_lv(struct volume_group *vg,
				       const char *name)
{
	struct list_head *tmp;
	struct logical_volume *lv;
	struct lv_list *ll;

	list_for_each(tmp, &vg->lvs) {
		ll = list_entry(tmp, struct lv_list, list);
		lv = &ll->lv;
		if (!strcmp(name, lv->name))
			return lv;
	}
	return NULL;
}

static struct physical_volume *_find_pv(struct volume_group *vg,
					struct device *dev)
{
	struct list_head *tmp;
	struct physical_volume *pv;
	struct pv_list *pl;

	list_for_each(tmp, &vg->pvs) {
		pl = list_entry(tmp, struct pv_list, list);
		pv = &pl->pv;
		if (dev == pv->dev)
			return pv;
	}
	return NULL;
}

static struct logical_volume *_add_lv(struct pool *mem,
				      struct volume_group *vg,
				      struct lv_disk *lvd)
{
	struct lv_list *ll = pool_alloc(mem, sizeof(*ll));
	struct logical_volume *lv;

	if (!ll) {
		stack;
		return 0;
	}
	lv = &ll->lv;

	memset(&lv->id.uuid, 0, sizeof(lv->id));
        if (!(lv->name = pool_strdup(mem, lvd->lv_name))) {
		stack;
		return 0;
	}

	// FIXME: finish
        //lv->access = lvd->lv_access;
        //lv->status = lvd->lv_status;
        lv->open = lvd->lv_open;
        lv->size = lvd->lv_size;
        lv->le_count = lvd->lv_allocated_le;
	lv->map = pool_alloc(mem, sizeof(struct pe_specifier) * lv->le_count);

	if (!lv->map) {
		stack;
		return 0;
	}

	list_add(&ll->list, &vg->lvs);
	vg->lv_count++;

	return lv;
}

static int _import_lvs(struct pool *mem, struct volume_group *vg,
		       struct list_head *pvs)
{
	struct list_head *tmp, *tmp2;
	struct disk_list *dl;
	struct lvd_list *ll;
	struct lv_disk *lvd;

	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);
		list_for_each(tmp2, &dl->lvs) {
			ll = list_entry(tmp2, struct lvd_list, list);
			lvd = &ll->lv;

			if (!_find_lv(vg, lvd->lv_name) &&
			    !_add_lv(mem, vg, lvd)) {
				stack;
				return 0;
			}
		}
	}

	return 1;
}

static int _fill_lv_array(struct logical_volume **lvs,
			  struct volume_group *vg, struct disk_list *dl)
{
	struct list_head *tmp;
	struct logical_volume *lv;
	int i = 0;

	list_for_each(tmp, &dl->lvs) {
		struct lvd_list *ll = list_entry(tmp, struct lvd_list, list);

		if (!(lv = _find_lv(vg, ll->lv.lv_name))) {
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
	struct logical_volume *lv, *lvs[MAX_LV];
	struct physical_volume *pv;
	struct pe_disk *e;
	int i;
	uint32_t lv_num, le;

	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);
		pv = _find_pv(vg, dl->dev);
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

			else if(lv_num > dl->pv.lv_cur) {
				log_err("invalid lv in extent map\n");
				return 0;

			} else {
				lv_num--;
				lv = lvs[lv_num];
				le = e[i].le_num;

				lv->map[le].pv = pv;
				lv->map[le].pe = i;
			}
		}
	}

	return 1;
}

static struct volume_group *_build_vg(struct pool *mem, struct list_head *pvs)
{
	struct volume_group *vg = pool_alloc(mem, sizeof(*vg));

	if (!vg) {
		stack;
		return 0;
	}

	memset(vg, 0, sizeof(*vg));

	INIT_LIST_HEAD(&vg->pvs);
	INIT_LIST_HEAD(&vg->lvs);

	if (!_import_vg(mem, vg, pvs))
		goto bad;

	if (!_import_pvs(mem, pvs, &vg->pvs, &vg->pv_count))
		goto bad;

	if (!_import_lvs(mem, vg, pvs))
		goto bad;

	if (!_import_extents(mem, vg, pvs))
		goto bad;

	return vg;

 bad:
	stack;
	pool_free(mem, vg);
	return NULL;
}

static struct volume_group *_vg_read(struct io_space *is, const char *vg_name)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list_head pvs;
	struct volume_group *vg;
	INIT_LIST_HEAD(&pvs);

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

#if 0
static struct disk_list *_flatten_pv(struct pool *mem, struct volume_group *vg,
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
#endif

static struct physical_volume *_pv_read(struct io_space *is,
					struct device *dev)
{
	struct pool *mem = pool_create(1024);
	struct physical_volume *pv;
	struct disk_list *dl;

	if (!mem) {
		stack;
		return NULL;
	}

	if (!(dl = read_pv(dev, mem, NULL))) {
		stack;
		goto bad;
	}

	if (!(pv = pool_alloc(is->mem, sizeof(*pv)))) {
		stack;
		goto bad;
	}

	if (!_import_pv(is->mem, dl, pv)) {
		stack;
		goto bad;
	}

	pool_destroy(mem);
	return pv;

 bad:
	pool_destroy(mem);
	return NULL;
}

static struct list_head *_get_pvs(struct io_space *is)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list_head pvs, *results;
	uint32_t count;

	if (!mem) {
		stack;
		return NULL;
	}

	if (!(results = pool_alloc(is->mem, sizeof(*results)))) {
		stack;
		return NULL;
	}

	INIT_LIST_HEAD(&pvs);
	INIT_LIST_HEAD(results);

	if (!read_pvs_in_vg(NULL, is->filter, mem, &pvs)) {
		stack;
		goto bad;
	}

	if (!_import_pvs(is->mem, &pvs, results, &count)) {
		stack;
		goto bad;
	}

	pool_destroy(mem);
	return results;

 bad:
	pool_destroy(mem);
	pool_free(mem, results);
	return NULL;
}

static int _find_vg_name(struct list_head *names, const char *vg)
{
	struct list_head *tmp;
	struct name_list *nl;

	list_for_each(tmp, names) {
		nl = list_entry(tmp, struct name_list, list);
		if (!strcmp(nl->name, vg))
			return 1;
	}

	return 0;
}

static struct list_head *_get_vgs(struct io_space *is)
{
	struct list_head *tmp, *pvs;
	struct list_head *names = pool_alloc(is->mem, sizeof(*names));
	struct name_list *nl;

	if (!names) {
		stack;
		return NULL;
	}

	INIT_LIST_HEAD(names);

	if (!(pvs = _get_pvs(is))) {
		stack;
		goto bad;
	}

	list_for_each(tmp, pvs) {
		struct pv_list *pvl = list_entry(tmp, struct pv_list, list);

		if (_find_vg_name(names, pvl->pv.vg_name))
			continue;

		if (!(nl = pool_alloc(is->mem, sizeof(*nl)))) {
			stack;
			goto bad;
		}

		if (!(nl->name = pool_strdup(is->mem, pvl->pv.vg_name))) {
			stack;
			goto bad;
		}

		list_add(&nl->list, names);
	}

	return names;

 bad:
	pool_free(is->mem, names);
	return NULL;
}

void _destroy(struct io_space *ios)
{
	dbg_free(ios->prefix);
	dbg_free(ios);
}

struct io_space *create_lvm1_format(const char *prefix, struct pool *mem,
				    struct dev_filter *filter)
{
	struct io_space *ios = dbg_malloc(sizeof(*ios));

	ios->get_vgs = _get_vgs;
	ios->get_pvs = _get_pvs;
	ios->pv_read = _pv_read;
	ios->pv_write = NULL;
	ios->vg_read = _vg_read;
	ios->vg_write = NULL;
	ios->destroy = _destroy;

	ios->prefix = dbg_malloc(strlen(prefix) + 1);
	if (!ios->prefix) {
		stack;
		dbg_free(ios);
		return 0;
	}
	strcpy(ios->prefix, prefix);

	ios->mem = mem;
	ios->filter = filter;
	ios->private = NULL;

	return ios;
}


