/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "disk-rep.h"
#include "dbg_malloc.h"
#include "pool.h"
#include "hash.h"
#include "list.h"
#include "log.h"
#include "display.h"

/* VG consistency checks */
static int _check_vgs(struct list *pvs)
{
	struct list *pvh;
	struct disk_list *dl = NULL;
	struct disk_list *first = NULL;

	int pv_count = 0;

	/* check all the vg's are the same */
	list_iterate(pvh, pvs) {
		dl = list_item(pvh, struct disk_list);

		if (!first)
			first = dl;
		else if (memcmp(&first->vgd, &dl->vgd, sizeof(first->vgd))) {
			log_err("VG data differs between PVs %s and %s",
				dev_name(first->dev), dev_name(dl->dev));
			return 0;
		}
		pv_count++;
	}

	/* On entry to fn, list known to be non-empty */
	if (!(pv_count == dl->vgd.pv_cur)) {
		log_error("Only %d out of %d PV(s) found for VG %s",
			  pv_count, dl->vgd.pv_cur, dl->pvd.vg_name);
		return 0;
	}

	return 1;
}

static struct volume_group *_build_vg(struct pool *mem, struct list *pvs)
{
	struct volume_group *vg = pool_alloc(mem, sizeof(*vg));
	struct disk_list *dl;

	if (!vg)
		goto bad;

	if (list_empty(pvs))
		goto bad;

	dl = list_item(pvs->n, struct disk_list);

	memset(vg, 0, sizeof(*vg));

	list_init(&vg->pvs);
	list_init(&vg->lvs);

	if (!_check_vgs(pvs))
		goto bad;

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
	struct list pvs;
	struct volume_group *vg;
	list_init(&pvs);

	if (!mem) {
		stack;
		return NULL;
	}

        /* Strip prefix if present */
        if (!strncmp(vg_name, is->prefix, strlen(is->prefix)))
                vg_name += strlen(is->prefix);

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

	list_init(&dl->uuids);
	list_init(&dl->lvds);

	if (!export_pv(&dl->pvd, pv) ||
	    !export_vg(&dl->vgd, vg) ||
	    !export_uuids(dl, vg) ||
	    !export_lvs(dl, vg, pv, prefix) ||
	    !calculate_layout(dl)) {
		stack;
		pool_free(mem, dl);
		return NULL;
	}

	return dl;
}

static int _flatten_vg(struct pool *mem, struct volume_group *vg,
		       struct list *pvs, const char *prefix,
		       struct dev_filter *filter)
{
	struct list *pvh;
	struct pv_list *pvl;
	struct disk_list *data;

	list_iterate(pvh, &vg->pvs) {
		pvl = list_item(pvh, struct pv_list);

		if (!(data = _flatten_pv(mem, vg, &pvl->pv, prefix))) {
			stack;
			return 0;
		}

		list_add(pvs, &data->list);
	}

	export_numbers(pvs, vg);
	export_pv_act(pvs);

	if (!export_vg_number(pvs, vg->name, filter)) {
		stack;
		return 0;
	}

	return 1;
}

static int _vg_write(struct io_space *is, struct volume_group *vg)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list pvs;
	int r = 0;

	if (!mem) {
		stack;
		return 0;
	}

	list_init(&pvs);

	r = (_flatten_vg(mem, vg, &pvs, is->prefix, is->filter) &&
	     write_pvds(&pvs));
	pool_destroy(mem);
	return r;
}

static struct physical_volume *_pv_read(struct io_space *is,
					const char *name)
{
	struct pool *mem = pool_create(1024);
	struct physical_volume *pv;
	struct disk_list *dl;
	struct device *dev;

        log_very_verbose("Reading physical volume data %s from disk", name);

	if (!mem) {
		stack;
		return NULL;
	}

	if (!(dev = dev_cache_get(name, is->filter))) {
		stack;
		goto bad;
	}

	if (!(dl = read_disk(dev, mem, NULL))) {
		stack;
		goto bad;
	}

	if (!(pv = pool_alloc(is->mem, sizeof(*pv)))) {
		stack;
		goto bad;
	}

	if (!import_pv(is->mem, dl->dev, pv, &dl->pvd)) {
		stack;
		goto bad;
	}

	pool_destroy(mem);
	return pv;

 bad:
	pool_destroy(mem);
	return NULL;
}

static struct list *_get_pvs(struct io_space *is)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list pvs, *results;
	uint32_t count;

	if (!mem) {
		stack;
		return NULL;
	}

	if (!(results = pool_alloc(is->mem, sizeof(*results)))) {
		stack;
		pool_destroy(mem);
		return NULL;
	}

	list_init(&pvs);
	list_init(results);

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
	pool_free(is->mem, results);
	pool_destroy(mem);
	return NULL;
}

static int _find_vg_name(struct list *names, const char *vg)
{
	struct list *nh;
	struct name_list *nl;

	list_iterate(nh, names) {
		nl = list_item(nh, struct name_list);
		if (!strcmp(nl->name, vg))
			return 1;
	}

	return 0;
}

static struct list *_get_vgs(struct io_space *is)
{
	struct list *pvh;
	struct list *pvs, *names = pool_alloc(is->mem, sizeof(*names));
	struct name_list *nl;

	if (!names) {
		stack;
		return NULL;
	}

	list_init(names);

	if (!(pvs = _get_pvs(is))) {
		stack;
		goto bad;
	}

	list_iterate(pvh, pvs) {
		struct pv_list *pvl = list_item(pvh, struct pv_list);

		if (!(*pvl->pv.vg_name) ||
	 	     _find_vg_name(names, pvl->pv.vg_name))
			continue;

		if (!(nl = pool_alloc(is->mem, sizeof(*nl)))) {
			stack;
			goto bad;
		}

		if (!(nl->name = pool_strdup(is->mem, pvl->pv.vg_name))) {
			stack;
			goto bad;
		}

		list_add(names, &nl->list);
	}

	if (list_empty(names))
		goto bad;

	return names;

 bad:
	pool_free(is->mem, names);
	return NULL;
}

static int _pv_setup(struct io_space *is, struct physical_volume *pv,
		     struct volume_group *vg)
{
	/*
	 * This works out pe_start and pe_count.
	 */
	if (!calculate_extent_count(pv)) {
		stack;
		return 0;
	}

	return 1;
}

static int _pv_write(struct io_space *is, struct physical_volume *pv)
{
	struct pool *mem;
	struct disk_list *dl;
	struct list pvs;

	list_init(&pvs);

	if (*pv->vg_name || pv->pe_allocated ) {
		log_error("Assertion failed: can't _pv_write non-orphan PV "
			  "(in VG %s)", pv->vg_name);
		return 0;
	}

	/* Ensure any residual PE structure is gone */
	pv->pe_size = pv->pe_count = pv->pe_start = 0;
	pv->status &= ~ALLOCATED_PV;

	if (!(mem = pool_create(1024))) {
		stack;
		return 0;
	}


	if (!(dl = pool_alloc(mem, sizeof(*dl)))) {
		stack;
		goto bad;
	}
	dl->mem = mem;
	dl->dev = pv->dev;

	if (!export_pv(&dl->pvd, pv)) {
		stack;
		goto bad;
	}

	list_add(&pvs, &dl->list);
	if (!write_pvds(&pvs)) {
		stack;
		goto bad;
	}

	pool_destroy(mem);
	return 1;

 bad:
	pool_destroy(mem);
	return 0;
}

int _vg_setup(struct io_space *is, struct volume_group *vg)
{
	/* just check max_pv and max_lv */
	if (vg->max_lv >= MAX_LV)
		vg->max_lv = MAX_LV - 1;

        if (vg->max_pv >= MAX_PV)
		vg->max_pv = MAX_PV - 1;

	if (vg->extent_size > MAX_PE_SIZE || vg->extent_size < MIN_PE_SIZE) {
		char *dummy, *dummy2;

		log_error("Extent size must be between %s and %s",
			(dummy = display_size(MIN_PE_SIZE / 2, SIZE_SHORT)),
			(dummy2 = display_size(MAX_PE_SIZE / 2, SIZE_SHORT)));

		dbg_free(dummy);
		dbg_free(dummy2);
		return 0;
	}

	if (vg->extent_size % MIN_PE_SIZE) {
		char *dummy;
		log_error("Extent size must be multiple of %s",
			(dummy = display_size(MIN_PE_SIZE / 2, SIZE_SHORT)));
		dbg_free(dummy);
		return 0;
	}

	/* Redundant? */
	if (vg->extent_size & (vg->extent_size - 1)) {
		log_error("Extent size must be power of 2");
		return 0;
	}

	return 1;
}

void _destroy(struct io_space *ios)
{
	dbg_free(ios->prefix);
	pool_destroy(ios->mem);
	dbg_free(ios);
}

struct io_space *create_lvm1_format(const char *prefix, struct pool *mem,
				    struct dev_filter *filter)
{
	struct io_space *ios = dbg_malloc(sizeof(*ios));

	if (!ios) {
		stack;
		return NULL;
	}

	ios->get_vgs = _get_vgs;
	ios->get_pvs = _get_pvs;
	ios->pv_read = _pv_read;
	ios->pv_setup = _pv_setup;
	ios->pv_write = _pv_write;
	ios->vg_read = _vg_read;
	ios->vg_setup = _vg_setup;
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
