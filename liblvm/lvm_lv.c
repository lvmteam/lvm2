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

#include "lib.h"
#include "metadata.h"
#include "lvm-string.h"
#include "defaults.h"
#include "segtype.h"
#include "locking.h"
#include "activate.h"
#include "lvm_misc.h"
#include "lvm2app.h"

/* FIXME Improve all the log messages to include context. Which VG/LV as a minimum? */

struct lvm_lv_create_params
{
	uint32_t magic;
	vg_t vg;
	struct lvcreate_params lvp;
};

#define LV_CREATE_PARAMS_MAGIC 0xFEED0001

static int _lv_check_handle(const lv_t lv, const int vg_writeable)
{
	if (!lv || !lv->vg || vg_read_error(lv->vg))
		return -1;
	if (vg_writeable && !vg_check_write_mode(lv->vg))
		return -1;
	return 0;
}

/* FIXME: have lib/report/report.c _disp function call lv_size()? */
uint64_t lvm_lv_get_size(const lv_t lv)
{
	return SECTOR_SIZE * lv_size(lv);
}

const char *lvm_lv_get_uuid(const lv_t lv)
{
	return lv_uuid_dup(lv);
}

const char *lvm_lv_get_name(const lv_t lv)
{
	return dm_pool_strndup(lv->vg->vgmem, (const char *)lv->name,
			       NAME_LEN+1);
}

const char *lvm_lv_get_attr(const lv_t lv)
{
	return lv_attr_dup(lv->vg->vgmem, lv);
}

const char *lvm_lv_get_origin(const lv_t lv)
{
	return lv_origin_dup(lv->vg->vgmem, lv);
}

struct lvm_property_value lvm_lv_get_property(const lv_t lv, const char *name)
{
	return get_property(NULL, NULL, lv, NULL, NULL, NULL, name);
}

struct lvm_property_value lvm_lvseg_get_property(const lvseg_t lvseg,
						 const char *name)
{
	return get_property(NULL, NULL, NULL, lvseg, NULL, NULL, name);
}

uint64_t lvm_lv_is_active(const lv_t lv)
{
	struct lvinfo info;
	if (lv_info(lv->vg->cmd, lv, 0, &info, 0, 0) &&
	    info.exists && info.live_table)
		return 1;
	return 0;
}

uint64_t lvm_lv_is_suspended(const lv_t lv)
{
	struct lvinfo info;
	if (lv_info(lv->vg->cmd, lv, 0, &info, 0, 0) &&
	    info.exists && info.suspended)
		return 1;
	return 0;
}

int lvm_lv_add_tag(lv_t lv, const char *tag)
{
	if (_lv_check_handle(lv, 1))
		return -1;
	if (!lv_change_tag(lv, tag, 1))
		return -1;
	return 0;
}


int lvm_lv_remove_tag(lv_t lv, const char *tag)
{
	if (_lv_check_handle(lv, 1))
		return -1;
	if (!lv_change_tag(lv, tag, 0))
		return -1;
	return 0;
}


struct dm_list *lvm_lv_get_tags(const lv_t lv)
{
	return tag_list_copy(lv->vg->vgmem, &lv->tags);
}

/* Set defaults for non-segment specific LV parameters */
static void _lv_set_default_params(struct lvcreate_params *lp,
				   vg_t vg, const char *lvname,
				   uint64_t extents)
{
	lp->zero = 1;
	lp->major = -1;
	lp->minor = -1;
	lp->activate = CHANGE_AY;
	lp->vg_name = vg->name;
	lp->lv_name = lvname; /* FIXME: check this for safety */
	lp->pvh = &vg->pvs;

	lp->extents = extents;
	lp->permission = LVM_READ | LVM_WRITE;
	lp->read_ahead = DM_READ_AHEAD_NONE;
	lp->alloc = ALLOC_INHERIT;
	dm_list_init(&lp->tags);
}

static struct segment_type * _get_segtype(struct cmd_context *cmd) {
	struct segment_type *rc = get_segtype_from_string(cmd, "striped");
	if (!rc) {
		log_error(INTERNAL_ERROR "Segtype striped not found.");
	}
	return rc;
}

/* Set default for linear segment specific LV parameters */
static int _lv_set_default_linear_params(struct cmd_context *cmd,
					  struct lvcreate_params *lp)
{
	if (!(lp->segtype = _get_segtype(cmd))) {
		return 0;
	}

	lp->stripes = 1;
	lp->stripe_size = DEFAULT_STRIPESIZE * 2;

	return 1;
}

/*
 * FIXME: This function should probably not commit to disk but require calling
 * lvm_vg_write.  However, this appears to be non-trivial change until
 * lv_create_single is refactored by segtype.
 */
lv_t lvm_vg_create_lv_linear(vg_t vg, const char *name, uint64_t size)
{
	struct lvcreate_params lp = { 0 };
	uint64_t extents;
	struct logical_volume *lv;

	if (vg_read_error(vg))
		return NULL;
	if (!vg_check_write_mode(vg))
		return NULL;

	if (!(extents = extents_from_size(vg->cmd, size / SECTOR_SIZE,
					  vg->extent_size))) {
		log_error("Unable to create LV without size.");
		return NULL;
	}

	_lv_set_default_params(&lp, vg, name, extents);
	if (!_lv_set_default_linear_params(vg->cmd, &lp))
		return_NULL;
	if (!(lv = lv_create_single(vg, &lp)))
		return_NULL;
	return (lv_t) lv;
}

/*
 * FIXME: This function should probably not commit to disk but require calling
 * lvm_vg_write.
 */
int lvm_vg_remove_lv(lv_t lv)
{
	if (!lv || !lv->vg || vg_read_error(lv->vg))
		return -1;
	if (!vg_check_write_mode(lv->vg))
		return -1;
	if (!lv_remove_single(lv->vg->cmd, lv, DONT_PROMPT))
		return -1;
	return 0;
}

int lvm_lv_activate(lv_t lv)
{
	if (!lv || !lv->vg || vg_read_error(lv->vg) || !lv->vg->cmd)
		return -1;

	/* FIXME: handle pvmove stuff later */
	if (lv->status & LOCKED) {
		log_error("Unable to activate locked LV");
		return -1;
	}

	/* FIXME: handle lvconvert stuff later */
	if (lv->status & CONVERTING) {
		log_error("Unable to activate LV with in-progress lvconvert");
		return -1;
	}

	if (lv_is_origin(lv)) {
		log_verbose("Activating logical volume \"%s\" "
			    "exclusively", lv->name);
		if (!activate_lv_excl(lv->vg->cmd, lv)) {
			/* FIXME Improve msg */
			log_error("Activate exclusive failed.");
			return -1;
		}
	} else {
		log_verbose("Activating logical volume \"%s\"",
			    lv->name);
		if (!activate_lv(lv->vg->cmd, lv)) {
			/* FIXME Improve msg */
			log_error("Activate failed.");
			return -1;
		}
	}
	return 0;
}

int lvm_lv_deactivate(lv_t lv)
{
	if (!lv || !lv->vg || vg_read_error(lv->vg) || !lv->vg->cmd)
		return -1;

	log_verbose("Deactivating logical volume \"%s\"", lv->name);
	if (!deactivate_lv(lv->vg->cmd, lv)) {
		log_error("Deactivate failed.");
		return -1;
	}
	return 0;
}

struct dm_list *lvm_lv_list_lvsegs(lv_t lv)
{
	struct dm_list *list;
	lvseg_list_t *lvseg;
	struct lv_segment *lvl;

	if (dm_list_empty(&lv->segments))
		return NULL;

	if (!(list = dm_pool_zalloc(lv->vg->vgmem, sizeof(*list)))) {
		log_errno(ENOMEM, "Memory allocation fail for dm_list.");
		return NULL;
	}
	dm_list_init(list);

	dm_list_iterate_items(lvl, &lv->segments) {
		if (!(lvseg = dm_pool_zalloc(lv->vg->vgmem, sizeof(*lvseg)))) {
			log_errno(ENOMEM,
				"Memory allocation fail for lvm_lvseg_list.");
			return NULL;
		}
		lvseg->lvseg = lvl;
		dm_list_add(list, &lvseg->list);
	}
	return list;
}

lv_t lvm_lv_from_name(vg_t vg, const char *name)
{
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (!strcmp(name, lvl->lv->name))
			return lvl->lv;
	}
	return NULL;
}

lv_t lvm_lv_from_uuid(vg_t vg, const char *uuid)
{
	struct lv_list *lvl;
	struct id id;

	if (strlen(uuid) < ID_LEN) {
		log_errno (EINVAL, "Invalid UUID string length");
		return NULL;
	}

	if (!id_read_format(&id, uuid)) {
		log_errno(EINVAL, "Invalid UUID format.");
		return NULL;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (id_equal(&vg->id, &lvl->lv->lvid.id[0]) &&
		    id_equal(&id, &lvl->lv->lvid.id[1]))
			return lvl->lv;
	}
	return NULL;
}

int lvm_lv_rename(lv_t lv, const char *new_name)
{
	if (!lv_rename(lv->vg->cmd, lv, new_name)) {
		/* FIXME Improve msg */
		log_error("LV rename failed.");
		return -1;
	}
	return 0;
}

int lvm_lv_resize(const lv_t lv, uint64_t new_size)
{
	struct lvresize_params lp = { 0 };

	lp.vg_name = lv->vg->name;
	lp.lv_name = lv->name;
	lp.sign = SIGN_NONE;
	lp.percent = PERCENT_NONE;
	lp.resize = LV_ANY;
	lp.size = new_size >> SECTOR_SHIFT;
	lp.ac_force = 1;	/* Assume the user has a good backup? */
	lp.sizeargs = 1;

	if (!lv_resize_prepare(lv->vg->cmd, lv, &lp, &lv->vg->pvs) ||
	    !lv_resize(lv->vg->cmd, lv, &lp, &lv->vg->pvs)) {
		/* FIXME Improve msg */
		log_error("LV resize failed.");
		/* FIXME Define consistent symbolic return codes */
		return -1;
	}

	return 0;
}

lv_t lvm_lv_snapshot(const lv_t lv, const char *snap_name,
						uint64_t max_snap_size)
{
	struct lvm_lv_create_params *lvcp = NULL;

	lvcp = lvm_lv_params_create_snapshot(lv, snap_name, max_snap_size);
	if (lvcp) {
		return lvm_lv_create(lvcp);
	}
	return NULL;
}

/* Set defaults for thin pool specific LV parameters */
static int _lv_set_pool_params(struct lvcreate_params *lp,
				vg_t vg, const char *pool,
				uint64_t extents, uint64_t meta_size)
{
	_lv_set_default_params(lp, vg, NULL, extents);

	lp->pool = pool;

	lp->create_thin_pool = 1;
	lp->segtype = get_segtype_from_string(vg->cmd, "thin-pool");
	lp->stripes = 1;

	if (!meta_size) {
		lp->poolmetadatasize = extents * vg->extent_size /
			(lp->chunk_size * (SECTOR_SIZE / 64));
		while ((lp->poolmetadatasize >
			(2 * DEFAULT_THIN_POOL_OPTIMAL_SIZE / SECTOR_SIZE)) &&
		       lp->chunk_size < DM_THIN_MAX_DATA_BLOCK_SIZE) {
			lp->chunk_size <<= 1;
			lp->poolmetadatasize >>= 1;
	         }
	} else
		lp->poolmetadatasize = meta_size;

	if (lp->poolmetadatasize % vg->extent_size)
		lp->poolmetadatasize +=
			vg->extent_size - lp->poolmetadatasize % vg->extent_size;

	if (!(lp->poolmetadataextents =
	      extents_from_size(vg->cmd, lp->poolmetadatasize / SECTOR_SIZE,
				vg->extent_size)))
		return_0;

	return 1;
}

lv_create_params_t lvm_lv_params_create_thin_pool(vg_t vg,
		const char *pool_name, uint64_t size, uint32_t chunk_size,
		uint64_t meta_size, lvm_thin_discards_t discard)
{
	uint64_t extents = 0;
	struct lvm_lv_create_params *lvcp = NULL;

	if (meta_size > (2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE)) {
		log_error("Invalid metadata size");
		return NULL;
	}

	if (meta_size &&
		meta_size < (2 * DEFAULT_THIN_POOL_MIN_METADATA_SIZE)) {
		log_error("Invalid metadata size");
		return NULL;
	}

	if (vg_read_error(vg))
		return NULL;

	if (!vg_check_write_mode(vg))
		return NULL;

	if (pool_name == NULL || !strlen(pool_name)) {
		log_error("pool_name invalid");
		return NULL;
	}

	if (!(extents = extents_from_size(vg->cmd, size / SECTOR_SIZE,
					  vg->extent_size))) {
		log_error("Unable to create LV thin pool without size.");
		return NULL;
	}

	lvcp = dm_pool_zalloc(vg->vgmem, sizeof (struct lvm_lv_create_params));

	if (lvcp) {
		lvcp->vg = vg;
		lvcp->lvp.discards = (thin_discards_t) discard;

		if (chunk_size)
			lvcp->lvp.chunk_size = chunk_size;
		else
			lvcp->lvp.chunk_size = DEFAULT_THIN_POOL_CHUNK_SIZE * 2;

		if (lvcp->lvp.chunk_size < DM_THIN_MIN_DATA_BLOCK_SIZE ||
				lvcp->lvp.chunk_size > DM_THIN_MAX_DATA_BLOCK_SIZE) {
			log_error("Invalid chunk_size");
			return NULL;
		}

		if (!_lv_set_pool_params(&lvcp->lvp, vg, pool_name, extents, meta_size))
			return_NULL;

		lvcp->magic = LV_CREATE_PARAMS_MAGIC;
	}
	return lvcp;
}

/* Set defaults for thin LV specific parameters */
static int _lv_set_thin_params(struct lvcreate_params *lp,
				vg_t vg, const char *pool,
				const char *lvname,
				uint64_t extents)
{
	_lv_set_default_params(lp, vg, lvname, extents);

	lp->thin = 1;
	lp->pool = pool;
	lp->segtype = get_segtype_from_string(vg->cmd, "thin");

	lp->voriginsize = extents * vg->extent_size;
	if (!(lp->voriginextents = extents_from_size(vg->cmd, lp->voriginsize,
						     vg->extent_size)))
		return_0;

	lp->stripes = 1;

	return 1;
}

lv_create_params_t lvm_lv_params_create_snapshot(const lv_t lv,
						 const char *snap_name,
						 uint64_t max_snap_size)
{
	uint64_t size = 0;
	uint64_t extents = 0;
	struct lvm_lv_create_params *lvcp = NULL;

	if (vg_read_error(lv->vg)) {
		return NULL;
	}

	if (!vg_check_write_mode(lv->vg))
			return NULL;

	if (snap_name == NULL || !strlen(snap_name)) {
		log_error("snap_name invalid");
		return NULL;
	}

	if (max_snap_size) {
		size = max_snap_size >> SECTOR_SHIFT;
		if (!(extents = extents_from_size(lv->vg->cmd, size, lv->vg->extent_size)))
			return_NULL;
	}

	if (!size && !lv_is_thin_volume(lv) ) {
		log_error("Origin is not thin, specify size of snapshot");
					return NULL;
	}

	lvcp = dm_pool_zalloc(lv->vg->vgmem, sizeof (struct lvm_lv_create_params));
	if (lvcp) {
		lvcp->vg = lv->vg;
		_lv_set_default_params(&lvcp->lvp, lv->vg, snap_name, extents);
		lvcp->lvp.snapshot = 1;


		if (size) {
			lvcp->lvp.segtype = _get_segtype(lvcp->vg->cmd);
			lvcp->lvp.chunk_size = 8;
		} else {
			lvcp->lvp.segtype = get_segtype_from_string(lv->vg->cmd, "thin");

			if (!lvcp->lvp.segtype) {
				log_error(INTERNAL_ERROR "Segtype thin not found.");
				return NULL;
			}

			lvcp->lvp.pool = first_seg(lv)->pool_lv->name;
		}

		lvcp->lvp.stripes = 1;
		lvcp->lvp.origin = lv->name;

		lvcp->magic = LV_CREATE_PARAMS_MAGIC;
	}

	return lvcp;
}


lv_create_params_t lvm_lv_params_create_thin(const vg_t vg, const char *pool_name,
									const char *lvname, uint64_t size)
{
	struct lvm_lv_create_params *lvcp = NULL;
	uint64_t extents = 0;

	/* precondition checks */
	if (vg_read_error(vg))
		return NULL;

	if (!vg_check_write_mode(vg))
		return NULL;

	if (pool_name == NULL || !strlen(pool_name)) {
		log_error("pool_name invalid");
		return NULL;
	}

	if (lvname == NULL || !strlen(lvname)) {
		log_error("lvname invalid");
		return NULL;
	}

	if (!(extents = extents_from_size(vg->cmd, size / SECTOR_SIZE,
			vg->extent_size))) {
		log_error("Unable to create thin LV without size.");
		return NULL;
	}

	lvcp = dm_pool_zalloc(vg->vgmem, sizeof (struct lvm_lv_create_params));
	if (lvcp) {
		lvcp->vg = vg;
		if (!_lv_set_thin_params(&lvcp->lvp, vg, pool_name, lvname, extents))
			return_NULL;

		lvcp->magic = LV_CREATE_PARAMS_MAGIC;
	}

	return lvcp;
}

struct lvm_property_value lvm_lv_params_get_property(
						const lv_create_params_t params,
						const char *name)
{
	struct lvm_property_value rc = {
		.is_valid = 0
	};

	if (params && params->magic == LV_CREATE_PARAMS_MAGIC) {
		rc = get_property(NULL, NULL, NULL, NULL, NULL, &params->lvp, name);
	} else {
		log_error("Invalid lv_create_params parameter");
	}

	return rc;
}

int lvm_lv_params_set_property(lv_create_params_t params, const char *name,
								struct lvm_property_value *prop)
{
	int rc = -1;

	if (params && params->magic == LV_CREATE_PARAMS_MAGIC) {
		rc = set_property(NULL, NULL, NULL, &params->lvp, name, prop);
	} else {
		log_error("Invalid lv_create_params parameter");
	}
	return rc;
}

lv_t lvm_lv_create(lv_create_params_t params)
{
	struct lv_list *lvl = NULL;

	if (params && params->magic == LV_CREATE_PARAMS_MAGIC) {
		if (!params->lvp.segtype) {
			log_error("segtype parameter is NULL");
			return_NULL;
		}
		if (!lv_create_single(params->vg, &params->lvp))
				return_NULL;

		/*
		 * In some case we are making a thin pool so lv_name is not valid, but
		 * pool is.
		 */
		if (!(lvl = find_lv_in_vg(params->vg,
				(params->lvp.lv_name) ? params->lvp.lv_name : params->lvp.pool)))
			return_NULL;
		return (lv_t) lvl->lv;
	}
	log_error("Invalid lv_create_params parameter");
	return NULL;
}
