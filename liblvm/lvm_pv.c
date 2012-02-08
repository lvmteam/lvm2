/*
 * Copyright (C) 2008,2009 Red Hat, Inc. All rights reserved.
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
#include "metadata.h"
#include "lvm-string.h"
#include "lvm_misc.h"
#include "lvm2app.h"

const char *lvm_pv_get_uuid(const pv_t pv)
{
	return pv_uuid_dup(pv);
}

const char *lvm_pv_get_name(const pv_t pv)
{
	return dm_pool_strndup(pv->vg->vgmem,
			       (const char *)pv_dev_name(pv), NAME_LEN + 1);
}

uint64_t lvm_pv_get_mda_count(const pv_t pv)
{
	return (uint64_t) pv_mda_count(pv);
}

uint64_t lvm_pv_get_dev_size(const pv_t pv)
{
	return (uint64_t) SECTOR_SIZE * pv_dev_size(pv);
}

uint64_t lvm_pv_get_size(const pv_t pv)
{
	return (uint64_t) SECTOR_SIZE * pv_size_field(pv);
}

uint64_t lvm_pv_get_free(const pv_t pv)
{
	return (uint64_t) SECTOR_SIZE * pv_free(pv);
}

struct lvm_property_value lvm_pv_get_property(const pv_t pv, const char *name)
{
	return get_property(pv, NULL, NULL, NULL, NULL, name);
}

struct lvm_property_value lvm_pvseg_get_property(const pvseg_t pvseg,
						 const char *name)
{
	return get_property(NULL, NULL, NULL, NULL, pvseg, name);
}

struct dm_list *lvm_pv_list_pvsegs(pv_t pv)
{
	struct dm_list *list;
	pvseg_list_t *pvseg;
	struct pv_segment *pvl;

	if (dm_list_empty(&pv->segments))
		return NULL;

	if (!(list = dm_pool_zalloc(pv->vg->vgmem, sizeof(*list)))) {
		log_errno(ENOMEM, "Memory allocation fail for dm_list.");
		return NULL;
	}
	dm_list_init(list);

	dm_list_iterate_items(pvl, &pv->segments) {
		if (!(pvseg = dm_pool_zalloc(pv->vg->vgmem, sizeof(*pvseg)))) {
			log_errno(ENOMEM,
				"Memory allocation fail for lvm_pvseg_list.");
			return NULL;
		}
		pvseg->pvseg = pvl;
		dm_list_add(list, &pvseg->list);
	}
	return list;
}

pv_t lvm_pv_from_name(vg_t vg, const char *name)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!strcmp(name, pv_dev_name(pvl->pv)))
			return pvl->pv;
	}
	return NULL;
}

pv_t lvm_pv_from_uuid(vg_t vg, const char *uuid)
{
	struct pv_list *pvl;
	struct id id;

	if (strlen(uuid) < ID_LEN) {
		log_errno (EINVAL, "Invalid UUID string length");
		return NULL;
	}

	if (!id_read_format(&id, uuid)) {
		log_errno(EINVAL, "Invalid UUID format.");
		return NULL;
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (id_equal(&id, &pvl->pv->id))
			return pvl->pv;
	}
	return NULL;
}


int lvm_pv_resize(const pv_t pv, uint64_t new_size)
{
	/* FIXME: add pv resize code here */
	log_error("NOT IMPLEMENTED YET");
	return -1;
}
