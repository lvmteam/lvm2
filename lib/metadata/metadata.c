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

#include <string.h>

int _add_pv_to_vg(struct format_instance *fi, struct volume_group *vg,
		  const char *pv_name)
{
	struct pv_list *pvl;
	struct physical_volume *pv;
	struct pool *mem = fi->cmd->mem;

	log_verbose("Adding physical volume '%s' to volume group '%s'",
		    pv_name, vg->name);

	if (!(pvl = pool_alloc(mem, sizeof (*pvl)))) {
		log_error("pv_list allocation for '%s' failed", pv_name);
		return 0;
	}

	if (!(pv = fi->ops->pv_read(fi, pv_name))) {
		log_error("Failed to read existing physical volume '%s'",
			  pv_name);
		return 0;
	}

	if (*pv->vg_name) {
		log_error("Physical volume '%s' is already in volume group "
			  "'%s'", pv_name, pv->vg_name);
		return 0;
	}

	/* FIXME check this */
	pv->exported = NULL;

	if (!(pv->vg_name = pool_strdup(mem, vg->name))) {
		log_error("vg->name allocation failed for '%s'", pv_name);
		return 0;
	}

	/* Units of 512-byte sectors */
	if (!dev_get_size(pv->dev, &pv->size)) {
		stack;
		return 0;
	}

	/* Units of 512-byte sectors */
	pv->pe_size = vg->extent_size;

	/*
	 * The next two fields should be corrected
	 * by fi->pv_setup.
	 */
	pv->pe_start = 0;
	pv->pe_count = pv->size / pv->pe_size;

	pv->pe_allocated = 0;

	if (!fi->ops->pv_setup(fi, pv, vg)) {
		log_debug("Format-specific setup of physical volume '%s' "
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

struct volume_group *vg_create(struct format_instance *fi, const char *vg_name,
			       uint32_t extent_size, int max_pv, int max_lv,
			       int pv_count, char **pv_names)
{
	struct volume_group *vg;
	struct pool *mem = fi->cmd->mem;

	if (!(vg = pool_alloc(mem, sizeof (*vg)))) {
		stack;
		return NULL;
	}

	/* is this vg name already in use ? */
	if (fi->ops->vg_read(fi, vg_name)) {
		log_err("A volume group called '%s' already exists.", vg_name);
		goto bad;
	}

	if (!id_create(&vg->id)) {
		log_err("Couldn't create uuid for volume group '%s'.",
			vg_name);
		goto bad;
	}

	/* Strip dev_dir if present */
	vg_name = strip_dir(vg_name, fi->cmd->dev_dir);

	vg->cmd = fi->cmd;

	if (!(vg->name = pool_strdup(mem, vg_name))) {
		stack;
		goto bad;
	}

	vg->status = (RESIZEABLE_VG | LVM_READ | LVM_WRITE);

	vg->extent_size = extent_size;
	vg->extent_count = 0;
	vg->free_count = 0;

	vg->max_lv = max_lv;
	vg->max_pv = max_pv;

	vg->pv_count = 0;
	list_init(&vg->pvs);

	vg->lv_count = 0;
	list_init(&vg->lvs);

	if (!fi->ops->vg_setup(fi, vg)) {
		log_error("Format specific setup of volume group '%s' failed.",
			  vg_name);
		goto bad;
	}

	/* attach the pv's */
	if (!vg_extend(fi, vg, pv_count, pv_names))
		goto bad;

	return vg;

 bad:
	pool_free(mem, vg);
	return NULL;
}

struct physical_volume *pv_create(struct format_instance *fi, 
				  const char *name,
				  struct id *id)
{
	struct pool *mem = fi->cmd->mem;
	struct physical_volume *pv = pool_alloc(mem, sizeof (*pv));

	if (!pv) {
		stack;
		return NULL;
	}

	if (!id)
		id_create(&pv->id);
	else
		memcpy(&pv->id, id, sizeof(*id));

	if (!(pv->dev = dev_cache_get(name, fi->cmd->filter))) {
		log_err("Couldn't find device '%s'", name);
		goto bad;
	}

	if (!(pv->vg_name = pool_alloc(mem, NAME_LEN))) {
		stack;
		goto bad;
	}

	*pv->vg_name = 0;
	pv->exported = NULL;
	pv->status = ALLOCATABLE_PV;

	if (!dev_get_size(pv->dev, &pv->size)) {
		log_err("Couldn't get size of device '%s'", name);
		goto bad;
	}

	pv->pe_size = 0;
	pv->pe_start = 0;
	pv->pe_count = 0;
	pv->pe_allocated = 0;
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
		/* FIXME check dev not name */
		if (!strcmp(dev_name(pvl->pv->dev), pv_name))
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
		if (!strcmp(lvl->lv.name, ptr))
			return lvl;
	}

	return NULL;
}

struct logical_volume *find_lv(struct volume_group *vg, const char *lv_name)
{
	struct lv_list *lvl = find_lv_in_vg(vg, lv_name);
	return lvl ? &lvl->lv : NULL;
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
