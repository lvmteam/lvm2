/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "log.h"
#include "pool.h"
#include "device.h"
#include "dev-cache.h"
#include "metadata.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "uuid.h"
#include "vgcache.h"

#include <string.h>

int _add_pv_to_vg(struct format_instance *fi, struct volume_group *vg,
		  const char *pv_name)
{
	struct pv_list *pvl;
	struct physical_volume *pv;
	struct pool *mem = fi->fmt->cmd->mem;

	log_verbose("Adding physical volume '%s' to volume group '%s'",
		    pv_name, vg->name);

	if (!(pvl = pool_alloc(mem, sizeof(*pvl)))) {
		log_error("pv_list allocation for '%s' failed", pv_name);
		return 0;
	}

	if (!(pv = pv_read(fi->fmt->cmd, pv_name))) {
		log_error("Failed to read existing physical volume '%s'",
			  pv_name);
		return 0;
	}

	if (*pv->vg_name) {
		log_error("Physical volume '%s' is already in volume group "
			  "'%s'", pv_name, pv->vg_name);
		return 0;
	}

	if (!(pv->vg_name = pool_strdup(mem, vg->name))) {
		log_error("vg->name allocation failed for '%s'", pv_name);
		return 0;
	}

	/* Units of 512-byte sectors */
	pv->pe_size = vg->extent_size;

	/* FIXME Do proper rounding-up alignment? */
	/* Reserved space for label; this holds 0 for PVs created by LVM1 */
	if (pv->pe_start < PE_ALIGN)
		pv->pe_start = PE_ALIGN;

	/*
	 * The next two fields should be corrected
	 * by fi->pv_setup.
	 */
	pv->pe_count = (pv->size - pv->pe_start) / pv->pe_size;

	pv->pe_alloc_count = 0;

	if (!fi->fmt->ops->pv_setup(fi, pv, vg)) {
		log_error("Format-specific setup of physical volume '%s' "
			  "failed.", pv_name);
		return 0;
	}

	if (find_pv_in_vg(vg, pv_name)) {
		log_error("Physical volume '%s' listed more than once.",
			  pv_name);
		return 0;
	}

	if (vg->pv_count == vg->max_pv) {
		log_error("No space for '%s' - volume group '%s' "
			  "holds max %d physical volume(s).", pv_name,
			  vg->name, vg->max_pv);
		return 0;
	}

	pvl->pv = pv;

	list_add(&vg->pvs, &pvl->list);
	vg->pv_count++;
	vg->extent_count += pv->pe_count;
	vg->free_count += pv->pe_count;

	return 1;
}

int vg_extend(struct format_instance *fi,
	      struct volume_group *vg, int pv_count, char **pv_names)
{
	int i;

	/* attach each pv */
	for (i = 0; i < pv_count; i++)
		if (!_add_pv_to_vg(fi, vg, pv_names[i])) {
			log_error("Unable to add physical volume '%s' to "
				  "volume group '%s'.", pv_names[i], vg->name);
			return 0;
		}

	return 1;
}

const char *strip_dir(const char *vg_name, const char *dev_dir)
{
	int len = strlen(dev_dir);
	if (!strncmp(vg_name, dev_dir, len))
		vg_name += len;

	return vg_name;
}

struct volume_group *vg_create(struct cmd_context *cmd, const char *vg_name,
			       uint32_t extent_size, int max_pv, int max_lv,
			       int pv_count, char **pv_names)
{
	struct volume_group *vg;
	struct pool *mem = cmd->mem;

	if (!(vg = pool_zalloc(mem, sizeof(*vg)))) {
		stack;
		return NULL;
	}

	/* is this vg name already in use ? */
	init_partial(1);
	if (vg_read(cmd, vg_name)) {
		log_err("A volume group called '%s' already exists.", vg_name);
		goto bad;
	}
	init_partial(0);

	if (!id_create(&vg->id)) {
		log_err("Couldn't create uuid for volume group '%s'.", vg_name);
		goto bad;
	}

	/* Strip dev_dir if present */
	vg_name = strip_dir(vg_name, cmd->dev_dir);

	vg->cmd = cmd;

	if (!(vg->name = pool_strdup(mem, vg_name))) {
		stack;
		goto bad;
	}

	vg->seqno = 0;

	vg->status = (RESIZEABLE_VG | LVM_READ | LVM_WRITE);
	vg->system_id = pool_alloc(mem, NAME_LEN);
	*vg->system_id = '\0';

	vg->extent_size = extent_size;
	vg->extent_count = 0;
	vg->free_count = 0;

	vg->max_lv = max_lv;
	vg->max_pv = max_pv;

	vg->pv_count = 0;
	list_init(&vg->pvs);

	vg->lv_count = 0;
	list_init(&vg->lvs);

	vg->snapshot_count = 0;
	list_init(&vg->snapshots);

	if (!(vg->fid = cmd->fmt->ops->create_instance(cmd->fmt, vg_name,
						       NULL))) {
		log_error("Failed to create format instance");
		goto bad;
	}

	if (!vg->fid->fmt->ops->vg_setup(vg->fid, vg)) {
		log_error("Format specific setup of volume group '%s' failed.",
			  vg_name);
		goto bad;
	}

	/* attach the pv's */
	if (!vg_extend(vg->fid, vg, pv_count, pv_names))
		goto bad;

	return vg;

      bad:
	pool_free(mem, vg);
	return NULL;
}

struct physical_volume *pv_create(struct format_instance *fid,
				  const char *name,
				  struct id *id, uint64_t size)
{
	struct pool *mem = fid->fmt->cmd->mem;
	struct physical_volume *pv = pool_alloc(mem, sizeof(*pv));

	if (!pv) {
		stack;
		return NULL;
	}

	if (!id)
		id_create(&pv->id);
	else
		memcpy(&pv->id, id, sizeof(*id));

	if (!(pv->dev = dev_cache_get(name, fid->fmt->cmd->filter))) {
		log_error("%s: Couldn't find device.", name);
		goto bad;
	}

	if (!(pv->vg_name = pool_alloc(mem, NAME_LEN))) {
		stack;
		goto bad;
	}

	*pv->vg_name = 0;
	pv->status = ALLOCATABLE_PV;

	if (!dev_get_size(pv->dev, &pv->size)) {
		log_error("%s: Couldn't get size.", name);
		goto bad;
	}

	if (size) {
		if (size > pv->size)
			log_print("WARNING: %s: Overriding real size. "
				  "You could lose data.", name);
		log_verbose("%s: Pretending size is %" PRIu64 " sectors.",
			    name, size);
		pv->size = size;
	}

	if (pv->size < PV_MIN_SIZE) {
		log_error("%s: Size must exceed minimum of %lu sectors.",
			  name, PV_MIN_SIZE);
		goto bad;
	}

	pv->pe_size = 0;
	pv->pe_start = 0;
	pv->pe_count = 0;
	pv->pe_alloc_count = 0;
	pv->fid = fid;

	if (!fid->fmt->ops->pv_setup(fid, pv, NULL)) {
		log_error("%s: Format-specific setup of physical volume "
			  "failed.", name);
		goto bad;
	}
	return pv;

      bad:
	pool_free(mem, pv);
	return NULL;
}

struct pv_list *find_pv_in_vg(struct volume_group *vg, const char *pv_name)
{
	struct list *pvh;
	struct pv_list *pvl;

	list_iterate(pvh, &vg->pvs) {
		pvl = list_item(pvh, struct pv_list);
		if (pvl->pv->dev == dev_cache_get(pv_name, vg->cmd->filter))
			return pvl;
	}

	return NULL;

}

struct lv_list *find_lv_in_vg(struct volume_group *vg, const char *lv_name)
{
	struct list *lvh;
	struct lv_list *lvl;
	const char *ptr;

	/* Use last component */
	if ((ptr = strrchr(lv_name, '/')))
		ptr++;
	else
		ptr = lv_name;

	list_iterate(lvh, &vg->lvs) {
		lvl = list_item(lvh, struct lv_list);
		if (!strcmp(lvl->lv->name, ptr))
			return lvl;
	}

	return NULL;
}

struct lv_list *find_lv_in_vg_by_lvid(struct volume_group *vg, union lvid *lvid)
{
	struct list *lvh;
	struct lv_list *lvl;

	list_iterate(lvh, &vg->lvs) {
		lvl = list_item(lvh, struct lv_list);
		if (!strncmp(lvl->lv->lvid.s, lvid->s, sizeof(*lvid)))
			return lvl;
	}

	return NULL;
}

struct logical_volume *find_lv(struct volume_group *vg, const char *lv_name)
{
	struct lv_list *lvl = find_lv_in_vg(vg, lv_name);
	return lvl ? lvl->lv : NULL;
}

struct physical_volume *find_pv(struct volume_group *vg, struct device *dev)
{
	struct list *pvh;
	struct physical_volume *pv;

	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;

		if (dev == pv->dev)
			return pv;
	}
	return NULL;
}

int vg_remove(struct volume_group *vg)
{
	struct list *mdah;
	void *mdl;

	if (!vg->fid->fmt->ops->vg_remove)
		return 1;

	/* FIXME Improve recovery situation? */
	/* Remove each copy of the metadata */
	list_iterate(mdah, &vg->fid->metadata_areas) {
		mdl = list_item(mdah, struct metadata_area)->metadata_locn;
		if (!vg->fid->fmt->ops->vg_remove(vg->fid, vg, mdl)) {
			stack;
			return 0;
		}
	}

	return 1;
}

int vg_write(struct volume_group *vg)
{
	struct list *mdah;
	void *mdl;

	vg->seqno++;

	/* Write to each copy of the metadata area */
	list_iterate(mdah, &vg->fid->metadata_areas) {
		mdl = list_item(mdah, struct metadata_area)->metadata_locn;
		if (!vg->fid->fmt->ops->vg_write(vg->fid, vg, mdl)) {
			stack;
			return 0;
		}
	}

	if (!vg->fid->fmt->ops->vg_commit)
		return 1;

	/* Commit to each copy of the metadata area */
	list_iterate(mdah, &vg->fid->metadata_areas) {
		mdl = list_item(mdah, struct metadata_area)->metadata_locn;
		if (!vg->fid->fmt->ops->vg_commit(vg->fid, vg, mdl)) {
			stack;
			return 0;
		}
	}

	return 1;
}

struct volume_group *vg_read(struct cmd_context *cmd, const char *vg_name)
{
	struct format_instance *fid;
	struct format_type *fmt;
	struct volume_group *vg, *correct_vg;
	struct list *mdah, *names;
	void *mdl;
	int inconsistent = 0, first_time = 1;

	/* create format instance with appropriate metadata area */
	if (!(fmt = vgcache_find_format(vg_name))) {
		/* Do full scan */
		if (!(names = get_vgs(cmd))) {
			stack;
			return NULL;
		}
		pool_free(cmd->mem, names);
		if (!(fmt = vgcache_find_format(vg_name))) {
			stack;
			return NULL;
		}
	}

	if (!(fid = fmt->ops->create_instance(fmt, vg_name, NULL))) {
		log_error("Failed to create format instance");
		return NULL;
	}

	/* Ensure contents of all metadata areas match - else do recovery */
	list_iterate(mdah, &fid->metadata_areas) {
		mdl = list_item(mdah, struct metadata_area)->metadata_locn;
		if (!(vg = fid->fmt->ops->vg_read(fid, vg_name, mdl))) {
			stack;
			return NULL;
		}
		if (first_time) {
			correct_vg = vg;
			first_time = 0;
			continue;
		}
		if (correct_vg->seqno != vg->seqno) {
			inconsistent = 1;
			if (vg->seqno > correct_vg->seqno)
				correct_vg = vg;
		}
	}

	if (inconsistent) {
		log_print("Inconsistent metadata copies found - updating "
			  "to use version %u", correct_vg->seqno);
		if (!vg_write(correct_vg)) {
			log_error("Automatic metadata correction failed");
			return NULL;
		}
	}

	vgcache_add(vg_name, correct_vg->id.uuid, NULL, fmt);

	return vg;
}

struct volume_group *vg_read_by_vgid(struct cmd_context *cmd, const char *vgid)
{
	char *vgname;
	struct list *vgs, *vgh;
	struct volume_group *vg;

	if (!(vgs = get_vgs(cmd))) {
		log_error("vg_read_by_vgid: get_vgs failed");
		return NULL;
	}

	list_iterate(vgh, vgs) {
		vgname = list_item(vgh, struct name_list)->name;
		if ((vg = vg_read(cmd, vgname)) &&
		    !strncmp(vg->id.uuid, vgid, ID_LEN)) return vg;
	}

	pool_free(cmd->mem, vgs);
	return NULL;
}

/* FIXME Use label functions instead of PV functions? */
struct physical_volume *pv_read(struct cmd_context *cmd, const char *pv_name)
{
	struct physical_volume *pv;

	if (!(pv = pool_zalloc(cmd->mem, sizeof(*pv)))) {
		log_error("pv_list allocation for '%s' failed", pv_name);
		return 0;
	}

	/* Member of a format1 VG? */
	if (!(cmd->fmt1->ops->pv_read(cmd->fmt1, pv_name, pv))) {
		log_error("Failed to read existing physical volume '%s'",
			  pv_name);
		return 0;
	}

	/* Member of a format_text VG? */
	if (!(cmd->fmtt->ops->pv_read(cmd->fmtt, pv_name, pv))) {
		log_error("Failed to read existing physical volume '%s'",
			  pv_name);
		return 0;
	}

	if (!pv->size)
		return NULL;
	else
		return pv;
}

struct list *get_vgs(struct cmd_context *cmd)
{
	struct list *names;

	if (!(names = pool_alloc(cmd->mem, sizeof(*names)))) {
		log_error("VG name list allocation failed");
		return NULL;
	}

	list_init(names);

	if (!cmd->fmt1->ops->get_vgs(cmd->fmt1, names) ||
	    !cmd->fmtt->ops->get_vgs(cmd->fmtt, names)) {
		pool_free(cmd->mem, names);
		return NULL;
	}

	return names;
}

struct list *get_pvs(struct cmd_context *cmd)
{
	struct list *results;

	if (!(results = pool_alloc(cmd->mem, sizeof(*results)))) {
		log_error("PV list allocation failed");
		return NULL;
	}

	list_init(results);

	/* fmtt modifies fmt1 output */
	if (!cmd->fmt1->ops->get_pvs(cmd->fmt1, results) ||
	    !cmd->fmtt->ops->get_pvs(cmd->fmtt, results)) {
		pool_free(cmd->mem, results);
		return NULL;
	}

	return results;
}

int pv_write(struct cmd_context *cmd, struct physical_volume *pv)
{
	struct list *mdah;
	void *mdl;

	/* Write to each copy of the metadata area */
	list_iterate(mdah, &pv->fid->metadata_areas) {
		mdl = list_item(mdah, struct metadata_area)->metadata_locn;
		if (!pv->fid->fmt->ops->pv_write(pv->fid, pv, mdl)) {
			stack;
			return 0;
		}
	}

	if (!pv->fid->fmt->ops->pv_commit)
		return 1;

	/* Commit to each copy of the metadata area */
	list_iterate(mdah, &pv->fid->metadata_areas) {
		mdl = list_item(mdah, struct metadata_area)->metadata_locn;
		if (!pv->fid->fmt->ops->pv_commit(pv->fid, pv, mdl)) {
			stack;
			return 0;
		}
	}

	return 1;
}
