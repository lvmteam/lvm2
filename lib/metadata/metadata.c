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

int _add_pv_to_vg(struct io_space *ios, struct volume_group *vg,
		  const char *pv_name)
{
	struct pv_list *pvl;
	struct physical_volume *pv;

	log_verbose("Adding physical volume '%s' to volume group '%s'",
		pv_name, vg->name);

	if (!(pvl = pool_alloc(ios->mem, sizeof (*pvl)))) {
		log_error("pv_list allocation for '%s' failed", pv_name);
		return 0;
	}

	if (!(pv = ios->pv_read(ios, pv_name))) {
		log_error("Failed to read existing physical volume '%s'",
			  pv_name);
		return 0;
	}

	if (*pv->vg_name) {
		log_error("Physical volume '%s' is already in volume group "
			  "'%s'", pv_name, pv->vg_name);
		return 0;
	}

	/* FIXME For LVM2, set on PV creation instead of here? */
	pv->status |= ALLOCATED_PV;

	/* FIXME check this */
	pv->exported = NULL;

	if (!(pv->vg_name = pool_strdup(ios->mem, vg->name))) {
		log_error("vg->name allocation failed for '%s'", pv_name);
		return 0;
	}

	/* FIXME Tie this to activation or not? */
	pv->status |= ACTIVE;

	/* Units of 512-byte sectors */
	if (!dev_get_size(pv->dev, &pv->size)) {
		stack;
		return 0;
	}

	/* Units of 512-byte sectors */
	pv->pe_size = vg->extent_size;

	/*
	 * The next two fields should be corrected
	 * by ios->pv_setup.
	 */
	pv->pe_start = 0;
	pv->pe_count = pv->size / pv->pe_size;

	pv->pe_allocated = 0;

	if (!ios->pv_setup(ios, pv, vg)) {
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

	memcpy(&pvl->pv, pv, sizeof (struct physical_volume));

	list_add(&vg->pvs, &pvl->list);
	vg->pv_count++;
	vg->extent_count += pv->pe_count;
	vg->free_count += pv->pe_count;

	return 1;
}

int vg_extend(struct io_space *ios, struct volume_group *vg, int pv_count,
	   char **pv_names)
{
	int i;

	/* attach each pv */
	for (i = 0; i < pv_count; i++)
		if (!_add_pv_to_vg(ios, vg, pv_names[i])) {
			log_error("Unable to add physical volume '%s' to "
				  "volume group '%s'.", pv_names[i], vg->name);
			return 0;
		}

	return 1;
}

struct volume_group *vg_create(struct io_space *ios, const char *vg_name,
			       uint32_t extent_size, int max_pv, int max_lv,
			       int pv_count, char **pv_names)
{
	struct volume_group *vg;

	if (!(vg = pool_alloc(ios->mem, sizeof (*vg)))) {
		stack;
		return NULL;
	}

	/* is this vg name already in use ? */
	if (ios->vg_read(ios, vg_name)) {
		log_err("A volume group called '%s' already exists.", vg_name);
		goto bad;
	}

	if (!id_create(&vg->id)) {
		log_err("Couldn't create uuid for volume group '%s'.",
			vg_name);
		goto bad;
	}

        /* Strip prefix if present */
        if (!strncmp(vg_name, ios->prefix, strlen(ios->prefix)))
                vg_name += strlen(ios->prefix);

	if (!(vg->name = pool_strdup(ios->mem, vg_name))) {
		stack;
		goto bad;
	}

	vg->status = (ACTIVE | EXTENDABLE_VG | LVM_READ | LVM_WRITE);

	vg->extent_size = extent_size;
	vg->extent_count = 0;
	vg->free_count = 0;

	vg->max_lv = max_lv;
	vg->max_pv = max_pv;

	vg->pv_count = 0;
	list_init(&vg->pvs);

	vg->lv_count = 0;
	list_init(&vg->lvs);

	if (!ios->vg_setup(ios, vg)) {
		log_error("Format specific setup of volume group '%s' failed.",
			  vg_name);
		goto bad;
	}

	/* attach the pv's */
	if (!vg_extend(ios, vg, pv_count, pv_names))
		goto bad;

	return vg;

      bad:
	pool_free(ios->mem, vg);
	return NULL;
}

struct physical_volume *pv_create(struct io_space *ios, const char *name)
{
	struct physical_volume *pv = pool_alloc(ios->mem, sizeof (*pv));

	if (!pv) {
		stack;
		return NULL;
	}

	id_create(&pv->id);
	if (!(pv->dev = dev_cache_get(name, ios->filter))) {
		log_err("Couldn't find device '%s'", name);
		goto bad;
	}

	if (!(pv->vg_name = pool_alloc(ios->mem, NAME_LEN))) {
		stack;
		goto bad;
	}

	*pv->vg_name = 0;
	pv->exported = NULL;
	pv->status = 0;

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
	pool_free(ios->mem, pv);
	return NULL;
}

struct list *find_pv_in_vg(struct volume_group *vg, const char *pv_name)
{
	struct list *pvh;
	struct pv_list *pv;

	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list);
		/* FIXME check dev not name */
		if (!strcmp(dev_name(pv->pv.dev), pv_name))
			return pvh;
	}

	return NULL;

}

struct list *find_lv_in_vg(struct volume_group *vg, const char *lv_name)
{
	struct list *lvh;
	const char *ptr;

	/* Use last component */
	if ((ptr = strrchr(lv_name, '/')))
		ptr++;
	else
		ptr = lv_name;

        list_iterate(lvh, &vg->lvs)
                if (!strcmp(list_item(lvh, struct lv_list)->lv.name, ptr))
                        return lvh;

        return NULL;
}

struct logical_volume *find_lv(struct volume_group *vg, const char *lv_name)
{
	struct list *lvh;

	if ((lvh = find_lv_in_vg(vg, lv_name)))
		return &list_item(lvh, struct lv_list)->lv;
	else
		return NULL;
}

struct physical_volume *_find_pv(struct volume_group *vg, struct device *dev)
{
	struct list *pvh;
        struct physical_volume *pv;
        struct pv_list *pl;

        list_iterate(pvh, &vg->pvs) {
                pl = list_item(pvh, struct pv_list);
                pv = &pl->pv;
                if (dev == pv->dev)
                        return pv;
        }
        return NULL;
}

int lv_remove(struct volume_group *vg, struct list *lvh)
{
	list_del(lvh);
	vg->lv_count--;

	return 1;
}

