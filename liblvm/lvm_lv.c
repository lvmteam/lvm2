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
#include "defaults.h"
#include "segtype.h"
#include "locking.h"
#include "activate.h"
#include "lvm_misc.h"
#include "lvm2app.h"

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

struct lvm_property_value lvm_lv_get_property(const lv_t lv, const char *name)
{
	return get_property(NULL, NULL, lv, NULL, NULL, name);
}

struct lvm_property_value lvm_lvseg_get_property(const lvseg_t lvseg,
						 const char *name)
{
	return get_property(NULL, NULL, NULL, lvseg, NULL, name);
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
	lp->activation_monitoring = DEFAULT_DMEVENTD_MONITOR;
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

/* Set default for linear segment specific LV parameters */
static void _lv_set_default_linear_params(struct cmd_context *cmd,
					  struct lvcreate_params *lp)
{
	lp->segtype = get_segtype_from_string(cmd, "striped");
	lp->stripes = 1;
	lp->stripe_size = DEFAULT_STRIPESIZE * 2;
}

/*
 * FIXME: This function should probably not commit to disk but require calling
 * lvm_vg_write.  However, this appears to be non-trivial change until
 * lv_create_single is refactored by segtype.
 */
lv_t lvm_vg_create_lv_linear(vg_t vg, const char *name, uint64_t size)
{
	struct lvcreate_params lp;
	uint64_t extents;
	struct lv_list *lvl;

	if (vg_read_error(vg))
		return NULL;
	if (!vg_check_write_mode(vg))
		return NULL;
	memset(&lp, 0, sizeof(lp));
	extents = extents_from_size(vg->cmd, size / SECTOR_SIZE,
				    vg->extent_size);
	_lv_set_default_params(&lp, vg, name, extents);
	_lv_set_default_linear_params(vg->cmd, &lp);
	if (!lv_create_single(vg, &lp))
		return NULL;
	lvl = find_lv_in_vg(vg, name);
	if (!lvl)
		return NULL;
	return (lv_t) lvl->lv;
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
			log_error("Activate exclusive failed.");
			return -1;
		}
	} else {
		log_verbose("Activating logical volume \"%s\"",
			    lv->name);
		if (!activate_lv(lv->vg->cmd, lv)) {
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
	if (strlen(uuid) >= ID_LEN) {
		if (!id_read_format(&id, uuid)) {
			log_errno(EINVAL, "Invalid UUID format");
			return NULL;
		}
	}
	dm_list_iterate_items(lvl, &vg->lvs) {
		if (id_equal(&vg->id, &lvl->lv->lvid.id[0]) &&
		    id_equal(&id, &lvl->lv->lvid.id[1]))
			return lvl->lv;
	}
	return NULL;
}
int lvm_lv_resize(const lv_t lv, uint64_t new_size)
{
	/* FIXME: add lv resize code here */
	log_error("NOT IMPLEMENTED YET");
	return -1;
}
