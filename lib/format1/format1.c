/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "disk-rep.h"
#include "limits.h"
#include "display.h"
#include "toolcontext.h"
#include "lvm1-label.h"
#include "format1.h"
#include "segtype.h"
#include "pv_alloc.h"

/* VG consistency checks */
static int _check_vgs(struct dm_list *pvs, struct volume_group *vg)
{
	struct dm_list *pvh, *t;
	struct disk_list *dl = NULL;
	struct disk_list *first = NULL;

	uint32_t pv_count = 0;
	uint32_t exported = 0;
	int first_time = 1;

	/*
	 * If there are exported and unexported PVs, ignore exported ones.
	 * This means an active VG won't be affected if disks are inserted
	 * bearing an exported VG with the same name.
	 */
	dm_list_iterate_items(dl, pvs) {
		if (first_time) {
			exported = dl->pvd.pv_status & VG_EXPORTED;
			first_time = 0;
			continue;
		}

		if (exported != (dl->pvd.pv_status & VG_EXPORTED)) {
			/* Remove exported PVs */
			dm_list_iterate_safe(pvh, t, pvs) {
				dl = dm_list_item(pvh, struct disk_list);
				if (dl->pvd.pv_status & VG_EXPORTED)
					dm_list_del(pvh);
			}
			break;
		}
	}

	/* Remove any PVs with VG structs that differ from the first */
	dm_list_iterate_safe(pvh, t, pvs) {
		dl = dm_list_item(pvh, struct disk_list);

		if (!first)
			first = dl;

		else if (memcmp(&first->vgd, &dl->vgd, sizeof(first->vgd))) {
			log_error("VG data differs between PVs %s and %s",
				  dev_name(first->dev), dev_name(dl->dev));
			log_debug_metadata("VG data on %s: %s %s %" PRIu32 " %" PRIu32
					   "  %" PRIu32 " %" PRIu32 " %" PRIu32 " %"
					   PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32
					   " %" PRIu32 " %" PRIu32 " %" PRIu32 " %"
					   PRIu32 " %" PRIu32 " %" PRIu32,
					   dev_name(first->dev), first->vgd.vg_uuid,
					   first->vgd.vg_name_dummy,
					   first->vgd.vg_number, first->vgd.vg_access,
					   first->vgd.vg_status, first->vgd.lv_max,
					   first->vgd.lv_cur, first->vgd.lv_open,
					   first->vgd.pv_max, first->vgd.pv_cur,
					   first->vgd.pv_act, first->vgd.dummy,
					   first->vgd.vgda, first->vgd.pe_size,
					   first->vgd.pe_total, first->vgd.pe_allocated,
					   first->vgd.pvg_total);
			log_debug_metadata("VG data on %s: %s %s %" PRIu32 " %" PRIu32
					   "  %" PRIu32 " %" PRIu32 " %" PRIu32 " %"
					   PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32
					   " %" PRIu32 " %" PRIu32 " %" PRIu32 " %"
					   PRIu32 " %" PRIu32 " %" PRIu32,
					   dev_name(dl->dev), dl->vgd.vg_uuid,
					   dl->vgd.vg_name_dummy, dl->vgd.vg_number,
					   dl->vgd.vg_access, dl->vgd.vg_status,
					   dl->vgd.lv_max, dl->vgd.lv_cur,
					   dl->vgd.lv_open, dl->vgd.pv_max,
					   dl->vgd.pv_cur, dl->vgd.pv_act, dl->vgd.dummy,
					   dl->vgd.vgda, dl->vgd.pe_size,
					   dl->vgd.pe_total, dl->vgd.pe_allocated,
					   dl->vgd.pvg_total);
			dm_list_del(pvh);
			return 0;
		}
		pv_count++;
	}

	/* On entry to fn, list known to be non-empty */
	if (pv_count != first->vgd.pv_cur) {
		log_error("%d PV(s) found for VG %s: expected %d",
			  pv_count, first->pvd.vg_name, first->vgd.pv_cur);
		vg->status |= PARTIAL_VG;
	}

	return 1;
}

static int _fix_partial_vg(struct volume_group *vg, struct dm_list *pvs)
{
	uint32_t extent_count = 0;
	struct disk_list *dl;
	struct dm_list *pvh;
	struct pv_list *pvl;
	struct lv_list *ll;
	struct lv_segment *seg;

	/*
	 * FIXME: code should remap missing segments to error segment.
	 * Also current mapping code allocates 1 segment per missing extent.
	 * For now bail out completely - allocated structures are not complete
	 */
	dm_list_iterate_items(ll, &vg->lvs)
		dm_list_iterate_items(seg, &ll->lv->segments) {

			/* area_count is always 1 here, s == 0 */
			if (seg_type(seg, 0) != AREA_PV)
				continue;

			if (seg_pv(seg, 0))
				continue;

			log_error("Partial mode support for missing lvm1 PVs and "
				  "partially available LVs is currently not implemented.");
			return 0;
	}

	dm_list_iterate(pvh, pvs) {
		dl = dm_list_item(pvh, struct disk_list);
		extent_count += dl->pvd.pe_total;
	}

	/* FIXME: move this to one place to pv_manip */
	if (!(pvl = dm_pool_zalloc(vg->vgmem, sizeof(*pvl))) ||
	    !(pvl->pv = dm_pool_zalloc(vg->vgmem, sizeof(*pvl->pv))))
		return_0;

	/* Use vg uuid with replaced first chars to "missing" as missing PV UUID */
	memcpy(&pvl->pv->id.uuid, vg->id.uuid, sizeof(pvl->pv->id.uuid));
	memcpy(&pvl->pv->id.uuid, "missing", 7);

	if (!(pvl->pv->vg_name = dm_pool_strdup(vg->vgmem, vg->name)))
		goto_out;
	memcpy(&pvl->pv->vgid, &vg->id, sizeof(vg->id));
	pvl->pv->status |= MISSING_PV;
	dm_list_init(&pvl->pv->tags);
	dm_list_init(&pvl->pv->segments);

	pvl->pv->pe_size = vg->extent_size;
	pvl->pv->pe_count = vg->extent_count - extent_count;
	if (!alloc_pv_segment_whole_pv(vg->vgmem, pvl->pv))
		goto_out;

	add_pvl_to_vgs(vg, pvl);
	log_debug_metadata("%s: partial VG, allocated missing PV using %d extents.",
			   vg->name, pvl->pv->pe_count);

	return 1;
out:
	dm_pool_free(vg->vgmem, pvl);
	return 0;
}

static struct volume_group *_format1_vg_read(struct format_instance *fid,
				     const char *vg_name,
				     struct metadata_area *mda __attribute__((unused)),
				     int single_device __attribute__((unused)))
{
	struct volume_group *vg;
	struct disk_list *dl;
	DM_LIST_INIT(pvs);

	/* Strip dev_dir if present */
	if (vg_name)
		vg_name = strip_dir(vg_name, fid->fmt->cmd->dev_dir);

	if (!(vg = alloc_vg("format1_vg_read", fid->fmt->cmd, NULL)))
		return_NULL;

	if (!read_pvs_in_vg(fid->fmt, vg_name, fid->fmt->cmd->filter,
			    vg->vgmem, &pvs))
		goto_bad;

	if (dm_list_empty(&pvs))
		goto_bad;

	if (!_check_vgs(&pvs, vg))
		goto_bad;

	dl = dm_list_item(pvs.n, struct disk_list);

	if (!import_vg(vg->vgmem, vg, dl))
		goto_bad;

	if (!import_pvs(fid->fmt, vg->vgmem, vg, &pvs))
		goto_bad;

	if (!import_lvs(vg->vgmem, vg, &pvs))
		goto_bad;

	if (!import_extents(fid->fmt->cmd, vg, &pvs))
		goto_bad;

	/* FIXME: workaround - temporary assignment of fid */
	vg->fid = fid;
	if (!import_snapshots(vg->vgmem, vg, &pvs)) {
		vg->fid = NULL;
		goto_bad;
	}
	vg->fid = NULL;

	/* Fix extents counts by adding missing PV if partial VG */
	if ((vg->status & PARTIAL_VG) && !_fix_partial_vg(vg, &pvs))
		goto_bad;

	vg_set_fid(vg, fid);

	return vg;

bad:
	release_vg(vg);

	return NULL;
}

static struct disk_list *_flatten_pv(struct format_instance *fid,
				     struct dm_pool *mem, struct volume_group *vg,
				     struct physical_volume *pv,
				     const char *dev_dir)
{
	struct disk_list *dl = dm_pool_alloc(mem, sizeof(*dl));

	if (!dl)
		return_NULL;

	dl->mem = mem;
	dl->dev = pv->dev;

	dm_list_init(&dl->uuids);
	dm_list_init(&dl->lvds);

	if (!export_pv(fid->fmt->cmd, mem, vg, &dl->pvd, pv) ||
	    !export_vg(&dl->vgd, vg) ||
	    !export_uuids(dl, vg) ||
	    !export_lvs(dl, vg, pv, dev_dir) || !calculate_layout(dl)) {
		dm_pool_free(mem, dl);
		return_NULL;
	}

	return dl;
}

static int _flatten_vg(struct format_instance *fid, struct dm_pool *mem,
		       struct volume_group *vg,
		       struct dm_list *pvds, const char *dev_dir,
		       struct dev_filter *filter)
{
	struct pv_list *pvl;
	struct disk_list *data;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(data = _flatten_pv(fid, mem, vg, pvl->pv, dev_dir)))
			return_0;

		dm_list_add(pvds, &data->list);
	}

	export_numbers(pvds, vg);
	export_pv_act(pvds);

	if (!export_vg_number(fid, pvds, vg->name, filter))
		return_0;

	return 1;
}

static int _format1_vg_write(struct format_instance *fid, struct volume_group *vg,
		     struct metadata_area *mda __attribute__((unused)))
{
	struct dm_pool *mem = dm_pool_create("lvm1 vg_write", VG_MEMPOOL_CHUNK);
	struct dm_list pvds;
	int r = 0;

	if (!mem)
		return_0;

	dm_list_init(&pvds);

	r = (_flatten_vg(fid, mem, vg, &pvds, fid->fmt->cmd->dev_dir,
			 fid->fmt->cmd->filter) &&
	     write_disks(fid->fmt, &pvds, 1));

	lvmcache_update_vg(vg, 0);
	dm_pool_destroy(mem);
	return r;
}

static int _format1_pv_read(const struct format_type *fmt, const char *pv_name,
		    struct physical_volume *pv, int scan_label_only __attribute__((unused)))
{
	struct dm_pool *mem = dm_pool_create("lvm1 pv_read", 1024);
	struct disk_list *dl;
	struct device *dev;
	int r = 0;

	log_very_verbose("Reading physical volume data %s from disk", pv_name);

	if (!mem)
		return_0;

	if (!(dev = dev_cache_get(pv_name, fmt->cmd->filter)))
		goto_out;

	if (!(dl = read_disk(fmt, dev, mem, NULL)))
		goto_out;

	if (!import_pv(fmt, fmt->cmd->mem, dl->dev, NULL, pv, &dl->pvd, &dl->vgd))
		goto_out;

	pv->fmt = fmt;

	r = 1;

      out:
	dm_pool_destroy(mem);
	return r;
}

static int _format1_pv_initialise(const struct format_type * fmt,
				  int64_t label_sector __attribute__((unused)),
				  unsigned long data_alignment __attribute__((unused)),
				  unsigned long data_alignment_offset __attribute__((unused)),
				  struct pvcreate_restorable_params *rp,
				  struct physical_volume * pv)
{
	if (pv->size > MAX_PV_SIZE)
		pv->size--;
	if (pv->size > MAX_PV_SIZE) {
		log_error("Physical volumes cannot be bigger than %s",
			  display_size(fmt->cmd, (uint64_t) MAX_PV_SIZE));
		return 0;
	}

	/* Nothing more to do if extent size isn't provided */
	if (!rp->extent_size)
		return 1;

	/*
	 * This works out pe_start and pe_count.
	 */
	if (!calculate_extent_count(pv, rp->extent_size, rp->extent_count, rp->pe_start))
		return_0;

	/* Retain existing extent locations exactly */
	if (((rp->pe_start || rp->extent_count) && (rp->pe_start != pv->pe_start)) ||
	    (rp->extent_count && (rp->extent_count != pv->pe_count))) {
		log_error("Metadata would overwrite physical extents");
		return 0;
	}

	return 1;
}

static int _format1_pv_setup(const struct format_type *fmt,
			     struct physical_volume *pv,
			     struct volume_group *vg)
{
	struct pvcreate_restorable_params rp = {.restorefile = NULL,
						.id = {{0}},
						.idp = NULL,
						.ba_start = 0,
						.ba_size = 0,
						.pe_start = 0,
						.extent_count = 0,
						.extent_size = vg->extent_size};

	return _format1_pv_initialise(fmt, -1, 0, 0, &rp, pv);
}

static int _format1_lv_setup(struct format_instance *fid, struct logical_volume *lv)
{
	uint64_t max_size = UINT_MAX;

	if (!*lv->lvid.s)
		lvid_from_lvnum(&lv->lvid, &lv->vg->id, find_free_lvnum(lv));

	if (lv->le_count > MAX_LE_TOTAL) {
		log_error("logical volumes cannot contain more than "
			  "%d extents.", MAX_LE_TOTAL);
		return 0;
	}
	if (lv->size > max_size) {
		log_error("logical volumes cannot be larger than %s",
			  display_size(fid->fmt->cmd, max_size));
		return 0;
	}

	return 1;
}

static int _format1_pv_write(const struct format_type *fmt, struct physical_volume *pv)
{
	struct dm_pool *mem;
	struct disk_list *dl;
	struct dm_list pvs;
	struct lvmcache_info *info;
	int pe_count, pe_size, pe_start;
	int r = 1;

	if (!(info = lvmcache_add(fmt->labeller, (char *) &pv->id, pv->dev,
				  pv->vg_name, NULL, 0)))
		return_0;

	lvmcache_update_pv(info, pv, fmt);
	lvmcache_del_mdas(info);
	lvmcache_del_das(info);
	lvmcache_del_bas(info);

	dm_list_init(&pvs);

	pe_count = pv->pe_count;
	pe_size = pv->pe_size;
	pe_start = pv->pe_start;

	/* Ensure any residual PE structure is gone */
	pv->pe_size = pv->pe_count = 0;
	pv->pe_start = LVM1_PE_ALIGN;

	if (!(mem = dm_pool_create("lvm1 pv_write", 1024)))
		return_0;

	if (!(dl = dm_pool_alloc(mem, sizeof(*dl))))
		goto_bad;

	dl->mem = mem;
	dl->dev = pv->dev;
	dm_list_init(&dl->uuids);
	dm_list_init(&dl->lvds);

	if (!export_pv(fmt->cmd, mem, NULL, &dl->pvd, pv))
		goto_bad;

	/* must be set to be able to zero gap after PV structure in
	   dev_write in order to make other disk tools happy */
	dl->pvd.pv_on_disk.base = METADATA_BASE;
	dl->pvd.pv_on_disk.size = PV_SIZE;
	dl->pvd.pe_on_disk.base = LVM1_PE_ALIGN << SECTOR_SHIFT;

	dm_list_add(&pvs, &dl->list);
	if (!write_disks(fmt, &pvs, 0))
		goto_bad;

	goto out;

      bad:
	r = 0;

      out:
	pv->pe_size = pe_size;
	pv->pe_count = pe_count;
	pv->pe_start = pe_start;

	dm_pool_destroy(mem);
	return r;
}

static int _format1_vg_setup(struct format_instance *fid, struct volume_group *vg)
{
	/* just check max_pv and max_lv */
	if (!vg->max_lv || vg->max_lv >= MAX_LV)
		vg->max_lv = MAX_LV - 1;

	if (!vg->max_pv || vg->max_pv >= MAX_PV)
		vg->max_pv = MAX_PV - 1;

	if (vg->extent_size > MAX_PE_SIZE || vg->extent_size < MIN_PE_SIZE) {
		log_error("Extent size must be between %s and %s",
			  display_size(fid->fmt->cmd, (uint64_t) MIN_PE_SIZE),
			  display_size(fid->fmt->cmd, (uint64_t) MAX_PE_SIZE));

		return 0;
	}

	if (vg->extent_size % MIN_PE_SIZE) {
		log_error("Extent size must be multiple of %s",
			  display_size(fid->fmt->cmd, (uint64_t) MIN_PE_SIZE));
		return 0;
	}

	/* Redundant? */
	if (vg->extent_size & (vg->extent_size - 1)) {
		log_error("Extent size must be power of 2");
		return 0;
	}

	return 1;
}

static int _format1_segtype_supported(struct format_instance *fid __attribute__((unused)),
				      const struct segment_type *segtype)
{
	if (!(segtype->flags & SEG_FORMAT1_SUPPORT))
		return_0;

	return 1;
}

static struct metadata_area_ops _metadata_format1_ops = {
	.vg_read = _format1_vg_read,
	.vg_write = _format1_vg_write,
};

static struct format_instance *_format1_create_instance(const struct format_type *fmt,
							const struct format_instance_ctx *fic)
{
	struct format_instance *fid;
	struct metadata_area *mda;

	if (!(fid = alloc_fid(fmt, fic)))
		return_NULL;

	/* Define a NULL metadata area */
	if (!(mda = dm_pool_zalloc(fid->mem, sizeof(*mda)))) {
		log_error("Unable to allocate metadata area structure "
			  "for lvm1 format");
		goto bad;
	}

	mda->ops = &_metadata_format1_ops;
	mda->metadata_locn = NULL;
	mda->status = 0;
	dm_list_add(&fid->metadata_areas_in_use, &mda->list);

	return fid;

bad:
	dm_pool_destroy(fid->mem);
	return NULL;
}

static void _format1_destroy_instance(struct format_instance *fid)
{
	if (--fid->ref_count <= 1)
		dm_pool_destroy(fid->mem);
}

static void _format1_destroy(struct format_type *fmt)
{
	if (fmt->orphan_vg)
		free_orphan_vg(fmt->orphan_vg);

	dm_free(fmt);
}

static struct format_handler _format1_ops = {
	.pv_read = _format1_pv_read,
	.pv_initialise = _format1_pv_initialise,
	.pv_setup = _format1_pv_setup,
	.pv_write = _format1_pv_write,
	.lv_setup = _format1_lv_setup,
	.vg_setup = _format1_vg_setup,
	.segtype_supported = _format1_segtype_supported,
	.create_instance = _format1_create_instance,
	.destroy_instance = _format1_destroy_instance,
	.destroy = _format1_destroy,
};

#ifdef LVM1_INTERNAL
struct format_type *init_lvm1_format(struct cmd_context *cmd)
#else				/* Shared */
struct format_type *init_format(struct cmd_context *cmd);
struct format_type *init_format(struct cmd_context *cmd)
#endif
{
	struct format_type *fmt = dm_malloc(sizeof(*fmt));
	struct format_instance_ctx fic;
	struct format_instance *fid;

	if (!fmt) {
		log_error("Failed to allocate format1 format type structure.");
		return NULL;
	}

	fmt->cmd = cmd;
	fmt->ops = &_format1_ops;
	fmt->name = FMT_LVM1_NAME;
	fmt->alias = NULL;
	fmt->orphan_vg_name = FMT_LVM1_ORPHAN_VG_NAME;
	fmt->features = FMT_RESTRICTED_LVIDS | FMT_ORPHAN_ALLOCATABLE |
			FMT_RESTRICTED_READAHEAD;
	fmt->private = NULL;

	dm_list_init(&fmt->mda_ops);

	if (!(fmt->labeller = lvm1_labeller_create(fmt))) {
		log_error("Couldn't create lvm1 label handler.");
		dm_free(fmt);
		return NULL;
	}

	if (!(label_register_handler(FMT_LVM1_NAME, fmt->labeller))) {
		log_error("Couldn't register lvm1 label handler.");
		fmt->labeller->ops->destroy(fmt->labeller);
		dm_free(fmt);
		return NULL;
	}

	if (!(fmt->orphan_vg = alloc_vg("format1_orphan", cmd, fmt->orphan_vg_name))) {
		log_error("Couldn't create lvm1 orphan VG.");
		dm_free(fmt);
		return NULL;
	}

	fic.type = FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = fmt->orphan_vg_name;
	fic.context.vg_ref.vg_id = NULL;

	if (!(fid = _format1_create_instance(fmt, &fic))) {
		_format1_destroy(fmt);
		return_NULL;
	}

	vg_set_fid(fmt->orphan_vg, fid);

	log_very_verbose("Initialised format: %s", fmt->name);

	return fmt;
}
