/*
 * Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
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

#include <stddef.h>
#include "lib.h"
#include "metadata.h"
#include "lvm-string.h"
#include "lvm_misc.h"
#include "lvm2app.h"
#include "locking.h"
#include "toolcontext.h"

struct lvm_pv_create_params
{
	uint32_t magic;
	lvm_t libh;
	const char *pv_name;
	struct pvcreate_params pv_p;
};

#define PV_CREATE_PARAMS_MAGIC 0xFEED0002

const char *lvm_pv_get_uuid(const pv_t pv)
{
	return pv_uuid_dup(pv);
}

const char *lvm_pv_get_name(const pv_t pv)
{
	return dm_pool_strndup(pv->vg->vgmem, pv_dev_name(pv), NAME_LEN);
}

uint64_t lvm_pv_get_mda_count(const pv_t pv)
{
	return (uint64_t) pv_mda_count(pv);
}

uint64_t lvm_pv_get_dev_size(const pv_t pv)
{
	return SECTOR_SIZE * pv_dev_size(pv);
}

uint64_t lvm_pv_get_size(const pv_t pv)
{
	return SECTOR_SIZE * pv_size_field(pv);
}

uint64_t lvm_pv_get_free(const pv_t pv)
{
	return SECTOR_SIZE * pv_free(pv);
}

struct lvm_property_value lvm_pv_get_property(const pv_t pv, const char *name)
{
	return get_property(pv, NULL, NULL, NULL, NULL, NULL, NULL, name);
}

struct lvm_property_value lvm_pvseg_get_property(const pvseg_t pvseg,
						 const char *name)
{
	return get_property(NULL, NULL, NULL, NULL, pvseg, NULL, NULL, name);
}

struct lvm_list_wrapper
{
	unsigned long magic;
	struct cmd_context *cmd;
	struct dm_list pvslist;
	struct dm_list vgslist;
};

int lvm_pv_remove(lvm_t libh, const char *pv_name)
{
	struct cmd_context *cmd = (struct cmd_context *)libh;

	if (!pvremove_single(cmd, pv_name, NULL, 0, 0))
		return -1;

	return 0;
}

#define PV_LIST_MAGIC 0xF005BA11

struct dm_list *lvm_list_pvs(lvm_t libh)
{
	struct lvm_list_wrapper *rc = NULL;
	struct cmd_context *cmd = (struct cmd_context *)libh;

	/*
	 * This memory will get cleared when the library handle
	 * gets closed, don't try to free is as it doesn't work
	 * like malloc/free do.
	 */
	if (!(rc = dm_pool_zalloc(cmd->mem, sizeof(*rc)))) {
		log_errno(ENOMEM, "Memory allocation fail for pv list.");
		return NULL;
	}

	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_WRITE, NULL)) {
		log_errno(ENOLCK, "Unable to obtain global lock.");
	} else {
		dm_list_init(&rc->pvslist);
		dm_list_init(&rc->vgslist);
		if( !get_pvs_perserve_vg(cmd, &rc->pvslist, &rc->vgslist) ) {
			return NULL;
		}

		/*
		 * If we have no PVs we still need to have access to cmd
		 * pointer in the free call.
		 */
		rc->cmd = cmd;
		rc->magic = PV_LIST_MAGIC;
	}

	return &rc->pvslist;
}

int lvm_list_pvs_free(struct dm_list *pvlist)
{
	struct lvm_list_wrapper *to_delete;
	struct vg_list *vgl;
	struct pv_list *pvl;

	if (pvlist) {
		to_delete = dm_list_struct_base(pvlist, struct lvm_list_wrapper, pvslist);
		if (to_delete->magic != PV_LIST_MAGIC) {
			log_errno(EINVAL, "Not a correct pvlist structure");
			return -1;
		}

		dm_list_iterate_items(vgl, &to_delete->vgslist) {
			release_vg(vgl->vg);
		}

		dm_list_iterate_items(pvl, &to_delete->pvslist)
			free_pv_fid(pvl->pv);

		unlock_vg(to_delete->cmd, VG_GLOBAL);
		to_delete->magic = 0xA5A5A5A5;
	}

	return 0;
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

	dm_list_iterate_items(pvl, &vg->pvs)
		if (!strcmp(name, pv_dev_name(pvl->pv)))
			return pvl->pv;

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

	dm_list_iterate_items(pvl, &vg->pvs)
		if (id_equal(&id, &pvl->pv->id))
			return pvl->pv;

	return NULL;
}

int lvm_pv_resize(const pv_t pv, uint64_t new_size)
{
	uint64_t size = new_size >> SECTOR_SHIFT;

	if (new_size % SECTOR_SIZE) {
		log_errno(EINVAL, "Size not a multiple of 512");
		return -1;
	}

	if (!vg_check_write_mode(pv->vg))
		return -1;

	if (!pv_resize_single(pv->vg->cmd, pv->vg, pv, size)) {
		log_error("PV re-size failed!");
		return -1;
	}

	return 0;
}

/*
 * Common internal code to create a parameter passing object
 */
static struct lvm_pv_create_params *_lvm_pv_params_create(
		lvm_t libh,
		const char *pv_name,
		struct lvm_pv_create_params *pvcp_in)
{
	struct lvm_pv_create_params *pvcp = NULL;
	const char *dev = NULL;
	struct cmd_context *cmd = (struct cmd_context *)libh;

	if (!pv_name || strlen(pv_name) == 0) {
		log_error("Invalid pv_name");
		return NULL;
	}

	if (!pvcp_in) {
		pvcp = dm_pool_zalloc(cmd->libmem, sizeof(struct lvm_pv_create_params));
	} else {
		pvcp = pvcp_in;
	}

	if (!pvcp) {
		return NULL;
	}

	dev = dm_pool_strdup(cmd->libmem, pv_name);
	if (!dev) {
		return NULL;
	}

	pvcreate_params_set_defaults(&pvcp->pv_p);
	pvcp->pv_p.yes = 1;
	pvcp->pv_p.force = DONT_PROMPT;
	pvcp->pv_name = dev;
	pvcp->libh = libh;
	pvcp->magic = PV_CREATE_PARAMS_MAGIC;

	return pvcp;
}

pv_create_params_t lvm_pv_params_create(lvm_t libh, const char *pv_name)
{
	return _lvm_pv_params_create(libh, pv_name, NULL);
}

struct lvm_property_value lvm_pv_params_get_property(
						const pv_create_params_t params,
						const char *name)
{
	struct lvm_property_value rc = {
		.is_valid = 0
	};

	if (params && params->magic == PV_CREATE_PARAMS_MAGIC) {
		rc = get_property(NULL, NULL, NULL, NULL, NULL, NULL, &params->pv_p,
							name);
	} else {
		log_error("Invalid pv_create_params parameter");
	}

	return rc;
}

int lvm_pv_params_set_property(pv_create_params_t params, const char *name,
								struct lvm_property_value *prop)
{
	int rc = -1;

	if (params && params->magic == PV_CREATE_PARAMS_MAGIC) {
		rc = set_property(NULL, NULL, NULL, NULL, &params->pv_p, name, prop);
	} else {
		log_error("Invalid pv_create_params parameter");
	}
	return rc;
}

static int _pv_create(pv_create_params_t params)
{
	struct cmd_context *cmd = (struct cmd_context *)params->libh;

	if (params->pv_p.size) {
		if (params->pv_p.size % SECTOR_SIZE) {
			log_errno(EINVAL, "Size not a multiple of 512");
			return -1;
		}
		params->pv_p.size = params->pv_p.size >> SECTOR_SHIFT;
	}

	if (!pvcreate_single(cmd, params->pv_name, &params->pv_p))
		return -1;
	return 0;
}

int lvm_pv_create(lvm_t libh, const char *pv_name, uint64_t size)
{
	struct lvm_pv_create_params pp;

	if (!_lvm_pv_params_create(libh, pv_name, &pp))
		return -1;

	pp.pv_p.size = size;

	return _pv_create(&pp);
}

int lvm_pv_create_adv(pv_create_params_t params)
{
	int rc = -1;

	if (params && params->magic == PV_CREATE_PARAMS_MAGIC) {
		rc = _pv_create(params);
	} else {
		log_error("Invalid pv_create_params parameter");
	}

	return rc;
}
