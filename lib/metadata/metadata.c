/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "log.h"
#include "pool.h"
#include "device.h"
#include "dev-cache.h"
#include "metadata.h"

#include <string.h>

int _add_pv_to_vg(struct io_space *ios, struct volume_group *vg,
		  const char *name)
{
	struct pv_list *pvl = pool_alloc(ios->mem, sizeof(*pvl));
	struct physical_volume *pv = &pvl->pv;

	if (!pv) {
		log_error("pv_list allocation for '%s' failed", name);
		return 0;
	}

	memset(pv, 0, sizeof(*pv));

	if (!(pv->dev = dev_cache_get(name, ios->filter))) {
		log_error("Physical volume '%s' not found.", name);
		return 0;
	}

	if (!(pv->vg_name = pool_strdup(ios->mem, vg->name))) {
		log_error("vg->name allocation failed for '%s'", name);
		return 0;
	}

	pv->exported = NULL;
        pv->status = ACTIVE;

	if (!dev_get_size(pv->dev, &pv->size)) {
		stack;
		return 0;
	}

	pv->pe_size = vg->extent_size;

	/*
	 * The next two fields should be corrected
	 * by ios->pv_setup.
	 */
	pv->pe_start = 0;
	pv->pe_count = pv->size / pv->pe_size;

	pv->pe_allocated = 0;

	if (!ios->pv_setup(ios, pv, vg)) {
		log_error("Format specific setup of physical volume '%s' "
			"failed.", name);
		return 0;
	}

	list_add(&pvl->list, &vg->pvs);
	vg->pv_count++;

	return 1;
}

struct volume_group *vg_create(struct io_space *ios, const char *name,
			       uint64_t extent_size, int max_pv, int max_lv,
			       int pv_count, char **pv_names)
{
	int i;
	struct volume_group *vg;

	if (!(vg = pool_alloc(ios->mem, sizeof(*vg)))) {
		stack;
		return NULL;
	}

	/* is this vg name already in use ? */
	if (ios->vg_read(ios, name)) {
		log_err("A volume group called '%s' already exists.", name);
		goto bad;
	}

	if (!id_create(&vg->id)) {
		log_err("Couldn't create uuid for volume group '%s'.", name);
		goto bad;
	}

	if (!(vg->name = pool_strdup(ios->mem, name))) {
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
	INIT_LIST_HEAD(&vg->pvs);

        vg->lv_count = 0;
	INIT_LIST_HEAD(&vg->lvs);

	if (!ios->vg_setup(ios, vg)) {
		log_err("Format specific setup of volume group '%s' failed.",
			name);
		goto bad;
	}

	/* attach the pv's */
	for (i = 0; i < pv_count; i++)
		if (!_add_pv_to_vg(ios, vg, pv_names[i])) {
			log_err("Unable to add physical volume '%s' to "
				"volume group '%s'.", pv_names[i], name);
			goto bad;
		}

	return vg;

 bad:
	pool_free(ios->mem, vg);
	return NULL;
}

struct physical_volume *pv_create(struct io_space *ios, const char *name)
{
	struct physical_volume *pv = pool_alloc(ios->mem, sizeof(*pv));

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



