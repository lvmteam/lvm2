/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "disk-rep.h"
#include "dbg_malloc.h"
#include "pool.h"
#include "hash.h"
#include "limits.h"
#include "list.h"
#include "log.h"
#include "display.h"
#include "toolcontext.h"

/* VG consistency checks */
static int _check_vgs(struct list *pvs, int *partial)
{
	struct list *pvh, *t;
	struct disk_list *dl = NULL;
	struct disk_list *first = NULL;

	int pv_count = 0;
	int exported = -1;

	*partial = 0;

	/*
	 * If there are exported and unexported PVs, ignore exported ones.
	 * This means an active VG won't be affected if disks are inserted
	 * bearing an exported VG with the same name.
	 */
	list_iterate(pvh, pvs) {
		dl = list_item(pvh, struct disk_list);

		if (exported < 0) {
			exported = dl->pvd.pv_status & VG_EXPORTED;
			continue;
		}

		if (exported != (dl->pvd.pv_status & VG_EXPORTED)) {
			/* Remove exported PVs */
			list_iterate_safe(pvh, t, pvs) {
				dl = list_item(pvh, struct disk_list);
				if (dl->pvd.pv_status & VG_EXPORTED)
					list_del(pvh);
			}
			break;
		}
	}

	/* Remove any PVs with VG structs that differ from the first */
	list_iterate_safe(pvh, t, pvs) {
		dl = list_item(pvh, struct disk_list);

		if (!first)
			first = dl;

		else if (memcmp(&first->vgd, &dl->vgd, sizeof(first->vgd))) {
			log_error("VG data differs between PVs %s and %s",
				  dev_name(first->dev), dev_name(dl->dev));
			list_del(pvh);
			if (partial_mode()) {
				*partial = 1;
				continue;
			}
			return 0;
		}
		pv_count++;
	}

	/* On entry to fn, list known to be non-empty */
	if (pv_count != dl->vgd.pv_cur) {
		log_error("%d PV(s) found for VG %s: expected %d",
			  pv_count, dl->pvd.vg_name, dl->vgd.pv_cur);
		if (!partial_mode())
			return 0;
		*partial = 1;
	}

	return 1;
}

static struct volume_group *_build_vg(struct format_instance *fid,
				      struct list *pvs)
{
	struct pool *mem = fid->fmt->cmd->mem;
	struct volume_group *vg = pool_alloc(mem, sizeof(*vg));
	struct disk_list *dl;
	int partial;

	if (!vg)
		goto bad;

	if (list_empty(pvs))
		goto bad;

	memset(vg, 0, sizeof(*vg));

	vg->cmd = fid->fmt->cmd;
	vg->fid = fid;
	vg->seqno = 0;
	list_init(&vg->pvs);
	list_init(&vg->lvs);
	list_init(&vg->snapshots);

	if (!_check_vgs(pvs, &partial))
		goto bad;

	dl = list_item(pvs->n, struct disk_list);

	if (!import_vg(mem, vg, dl, partial))
		goto bad;

	if (!import_pvs(fid, mem, vg, pvs, &vg->pvs, &vg->pv_count))
		goto bad;

	if (!import_lvs(mem, vg, pvs))
		goto bad;

	if (!import_extents(mem, vg, pvs))
		goto bad;

	if (!import_snapshots(mem, vg, pvs))
		goto bad;

	return vg;

      bad:
	stack;
	pool_free(mem, vg);
	return NULL;
}

static struct volume_group *_vg_read(struct format_instance *fid,
				     const char *vg_name, void *mda)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list pvs;
	struct volume_group *vg = NULL;
	list_init(&pvs);

	if (!mem) {
		stack;
		return NULL;
	}

	/* Strip dev_dir if present */
	vg_name = strip_dir(vg_name, fid->fmt->cmd->dev_dir);

	if (!read_pvs_in_vg
	    (fid->fmt, vg_name, fid->fmt->cmd->filter, mem, &pvs)) {
		stack;
		goto bad;
	}

	if (!(vg = _build_vg(fid, &pvs))) {
		stack;
		goto bad;
	}

      bad:
	pool_destroy(mem);
	return vg;
}

static struct disk_list *_flatten_pv(struct pool *mem, struct volume_group *vg,
				     struct physical_volume *pv,
				     const char *dev_dir)
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

	if (!export_pv(mem, vg, &dl->pvd, pv) ||
	    !export_vg(&dl->vgd, vg) ||
	    !export_uuids(dl, vg) ||
	    !export_lvs(dl, vg, pv, dev_dir) ||
	    !calculate_layout(dl)) {
		stack;
		pool_free(mem, dl);
		return NULL;
	}

	return dl;
}

static int _flatten_vg(struct format_instance *fid, struct pool *mem,
		       struct volume_group *vg,
		       struct list *pvds, const char *dev_dir,
		       struct dev_filter *filter)
{
	struct list *pvh;
	struct pv_list *pvl;
	struct disk_list *data;

	list_iterate(pvh, &vg->pvs) {
		pvl = list_item(pvh, struct pv_list);

		if (!(data = _flatten_pv(mem, vg, pvl->pv, dev_dir))) {
			stack;
			return 0;
		}

		list_add(pvds, &data->list);
	}

	export_numbers(pvds, vg);
	export_pv_act(pvds);

	if (!export_vg_number(fid, pvds, vg->name, filter)) {
		stack;
		return 0;
	}

	return 1;
}

static int _vg_write(struct format_instance *fid, struct volume_group *vg,
		     void *mdl)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list pvds;
	int r = 0;

	if (!mem) {
		stack;
		return 0;
	}

	if (vg->status & PARTIAL_VG) {
		log_error("Cannot change metadata for partial volume group %s",
			  vg->name);
		return 0;
	}

	list_init(&pvds);

	r = (_flatten_vg(fid, mem, vg, &pvds, fid->fmt->cmd->dev_dir,
			 fid->fmt->cmd->filter) &&
	     write_disks(fid->fmt, &pvds));
	pool_destroy(mem);
	return r;
}

int _pv_read(struct format_type *fmt, const char *name,
	     struct physical_volume *pv)
{
	struct pool *mem = pool_create(1024);
	struct disk_list *dl;
	struct device *dev;
	int r = 0;

	log_very_verbose("Reading physical volume data %s from disk", name);

	if (!mem) {
		stack;
		return 0;
	}

	if (!(dev = dev_cache_get(name, fmt->cmd->filter))) {
		stack;
		goto out;
	}

	if (!(dl = read_disk(fmt, dev, mem, NULL))) {
		stack;
		goto out;
	}

	if (!import_pv(fmt->cmd->mem, dl->dev, NULL, pv, &dl->pvd)) {
		stack;
		goto out;
	}

	pv->fid = fmt->ops->create_instance(fmt, NULL, NULL);

	r = 1;

      out:
	pool_destroy(mem);
	return r;
}

static struct list *_get_pvs(struct format_type *fmt, struct list *results)
{
	struct pool *mem = pool_create(1024 * 10);
	struct list pvs;
	uint32_t count;

	if (!mem) {
		stack;
		return NULL;
	}

	list_init(&pvs);

	if (!read_pvs_in_vg(fmt, NULL, fmt->cmd->filter, mem, &pvs)) {
		stack;
		goto bad;
	}

	if (!import_pvs(NULL, fmt->cmd->mem, NULL, &pvs, results, &count)) {
		stack;
		goto bad;
	}

	pool_destroy(mem);
	return results;

      bad:
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

static struct list *_get_vgs(struct format_type *fmt, struct list *names)
{
	struct list *pvh;
	struct list *pvs;
	struct name_list *nl;

	if (!(pvs = pool_alloc(fmt->cmd->mem, sizeof(*pvs)))) {
		log_error("PV list allocation failed");
		goto err;
	}

	list_init(pvs);

	if (!_get_pvs(fmt, pvs)) {
		stack;
		goto err;
	}

	list_iterate(pvh, pvs) {
		struct pv_list *pvl = list_item(pvh, struct pv_list);

		if (!(*pvl->pv->vg_name) ||
		    _find_vg_name(names, pvl->pv->vg_name))
			continue;

		if (!(nl = pool_alloc(fmt->cmd->mem, sizeof(*nl)))) {
			stack;
			goto err;
		}

		if (!(nl->name = pool_strdup(fmt->cmd->mem, pvl->pv->vg_name))) {
			stack;
			goto err;
		}

		list_add(names, &nl->list);
	}

	if (list_empty(names))
		goto err;

	return names;

      err:
	pool_free(fmt->cmd->mem, pvs);
	return NULL;
}

static int _pv_setup(struct format_instance *fid, struct physical_volume *pv,
		     struct volume_group *vg)
{
	/* setup operations for the PV structure */
	if (pv->size > MAX_PV_SIZE)
		pv->size--;
	if (pv->size > MAX_PV_SIZE) {
		/* FIXME Limit hardcoded */
		log_error("Physical volumes cannot be bigger than 2TB");
		return 0;
	}

	/* Nothing more to do if pe_size isn't known */
	if (!vg)
		return 1;

	/*
	 * This works out pe_start and pe_count.
	 */
	if (!calculate_extent_count(pv)) {
		stack;
		return 0;
	}

	return 1;
}

static int _find_free_lvnum(struct logical_volume *lv)
{
	int lvnum_used[MAX_LV];
	int i = 0;
	struct list *lvh;
	struct lv_list *lvl;

	memset(&lvnum_used, 0, sizeof(lvnum_used));

	list_iterate(lvh, &lv->vg->lvs) {
		lvl = list_item(lvh, struct lv_list);
		lvnum_used[lvnum_from_lvid(&lvl->lv->lvid)] = 1;
	}

	while (lvnum_used[i])
		i++;

	return i;
}

static int _lv_setup(struct format_instance *fid, struct logical_volume *lv)
{
	uint64_t max_size = UINT_MAX;

	if (!*lv->lvid.s)
		lvid_from_lvnum(&lv->lvid, &lv->vg->id, _find_free_lvnum(lv));

	if (lv->le_count > MAX_LE_TOTAL) {
		log_error("logical volumes cannot contain more than "
			  "%d extents.", MAX_LE_TOTAL);
		return 0;
	}
	if (lv->size > max_size) {
		char *dummy = display_size(max_size, SIZE_SHORT);
		log_error("logical volumes cannot be larger than %s", dummy);
		dbg_free(dummy);
		return 0;
	}

	return 1;
}

static int _pv_write(struct format_instance *fid, struct physical_volume *pv,
		     void *mdl)
{
	struct pool *mem;
	struct disk_list *dl;
	struct list pvs;

	list_init(&pvs);

	if (*pv->vg_name || pv->pe_alloc_count) {
		log_error("Assertion failed: can't _pv_write non-orphan PV "
			  "(in VG %s)", pv->vg_name);
		return 0;
	}

	/* Ensure any residual PE structure is gone */
	pv->pe_size = pv->pe_count = 0;
	pv->pe_start = PE_ALIGN;

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

	if (!export_pv(mem, NULL, &dl->pvd, pv)) {
		stack;
		goto bad;
	}

	/* must be set to be able to zero gap after PV structure in
	   dev_write in order to make other disk tools happy */
	dl->pvd.pv_on_disk.base = METADATA_BASE;
	dl->pvd.pv_on_disk.size = PV_SIZE;
	dl->pvd.pe_on_disk.base = PE_ALIGN * SECTOR_SIZE;

	list_add(&pvs, &dl->list);
	if (!write_disks(fid->fmt, &pvs)) {
		stack;
		goto bad;
	}

	pool_destroy(mem);
	return 1;

      bad:
	pool_destroy(mem);
	return 0;
}

int _vg_setup(struct format_instance *fid, struct volume_group *vg)
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

struct format_instance *_create_instance(struct format_type *fmt,
					 const char *vgname, void *private)
{
	struct format_instance *fid;
	struct metadata_area *mda;

	if (!(fid = pool_alloc(fmt->cmd->mem, sizeof(*fid)))) {
		stack;
		return NULL;
	}

	fid->fmt = fmt;
	list_init(&fid->metadata_areas);

	/* Define a NULL metadata area */
	if (!(mda = pool_alloc(fmt->cmd->mem, sizeof(*mda)))) {
		stack;
		pool_free(fmt->cmd->mem, fid);
		return NULL;
	}

	mda->metadata_locn = NULL;
	list_add(&fid->metadata_areas, &mda->list);

	return fid;
}

void _destroy_instance(struct format_instance *fid)
{
	return;
}

void _destroy(struct format_type *fmt)
{
	dbg_free(fmt);
}

static struct format_handler _format1_ops = {
	get_vgs:	_get_vgs,
	get_pvs:	_get_pvs,
	pv_read:	_pv_read,
	pv_setup:	_pv_setup,
	pv_write:	_pv_write,
	lv_setup:	_lv_setup,
	vg_read:	_vg_read,
	vg_setup:	_vg_setup,
	vg_write:	_vg_write,
	create_instance:_create_instance,
	destroy_instance:_destroy_instance,
	destroy:	_destroy,
};

struct format_type *create_lvm1_format(struct cmd_context *cmd)
{
	struct format_type *fmt = dbg_malloc(sizeof(*fmt));

	if (!fmt) {
		stack;
		return NULL;
	}

	fmt->cmd = cmd;
	fmt->ops = &_format1_ops;
	fmt->name = FMT_LVM1_NAME;
	fmt->features = 0;
	fmt->private = NULL;

	return fmt;
}
