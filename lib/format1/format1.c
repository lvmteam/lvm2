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

static int _check_vgs(struct list_head *pvs)
{
	struct list_head *tmp;
	struct disk_list *dl;
	struct vg_disk *first = NULL;

	/* check all the vg's are the same */
	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);

		if (!first)
			first = &dl->vg;
		else if (memcmp(first, &dl->vg, sizeof(*first))) {
			log_err("vg data differs on pvs\n");
			return 0;
		}
	}

	return 1;
}

static struct volume_group *_build_vg(struct pool *mem, struct list_head *pvs)
{
	struct volume_group *vg = pool_alloc(mem, sizeof(*vg));
	struct disk_list *dl = list_entry(pvs->next, struct disk_list, list);

	if (!dl) {
		log_err("no pv's in volume group");
		return NULL;
	}

	if (!vg) {
		stack;
		return NULL;
	}

	memset(vg, 0, sizeof(*vg));

	INIT_LIST_HEAD(&vg->pvs);
	INIT_LIST_HEAD(&vg->lvs);

	if (!_check_vgs(pvs)) {
		stack;
		return NULL;
	}

	if (!import_vg(mem, vg, dl))
		goto bad;

	if (!import_pvs(mem, pvs, &vg->pvs, &vg->pv_count))
		goto bad;

	if (!import_lvs(mem, vg, pvs))
		goto bad;

	if (!import_extents(mem, vg, pvs))
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

	if (!(vg = _build_vg(is->mem, &pvs)))
		stack;

	pool_destroy(mem);
	return vg;
}

static struct disk_list *_flatten_pv(struct pool *mem, struct volume_group *vg,
				     struct physical_volume *pv,
				     const char *prefix)
{
	struct disk_list *dl = pool_alloc(mem, sizeof(*dl));

	if (!dl) {
		stack;
		return NULL;
	}

	dl->mem = mem;
	dl->dev = pv->dev;

	INIT_LIST_HEAD(&dl->uuids);
	INIT_LIST_HEAD(&dl->lvs);

	if (!export_pv(&dl->pv, pv) ||
	    !export_vg(&dl->vg, vg) ||
	    !export_uuids(dl, vg) ||
	    !export_lvs(dl, vg, pv, prefix)) {
		stack;
		return NULL;
	}

	return dl;
}

static int _flatten_vg(struct pool *mem, struct volume_group *vg,
		       struct list_head *pvs, const char *prefix)
{
	struct list_head *tmp;
	struct pv_list *pvl;
	struct disk_list *data;

	list_for_each(tmp, &vg->pvs) {
		pvl = list_entry(tmp, struct pv_list, list);

		if (!(data = _flatten_pv(mem, vg, &pvl->pv, prefix))) {
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

	r = _flatten_vg(mem, vg, &pvs, is->prefix) && write_pvs(&pvs);
	pool_destroy(mem);
	return r;
}

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

	if (!import_pv(is->mem, dl->dev, pv, &dl->pv)) {
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

	if (!import_pvs(is->mem, &pvs, results, &count)) {
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
	ios->vg_write = _vg_write;
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
