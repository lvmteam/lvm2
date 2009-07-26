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
#include "lvm.h"
#include "toolcontext.h"
#include "metadata-exported.h"
#include "archiver.h"
#include "locking.h"
#include "lvm-string.h"
#include "lvmcache.h"
#include "metadata.h"

#include <errno.h>
#include <string.h>

vg_t *lvm_vg_create(lvm_t libh, const char *vg_name)
{
	vg_t *vg;

	vg = vg_create((struct cmd_context *)libh, vg_name);
	/* FIXME: error handling is still TBD */
	if (vg_read_error(vg)) {
		vg_release(vg);
		return NULL;
	}
	return vg;
}

int lvm_vg_extend(vg_t *vg, const char *device)
{
	if (vg_read_error(vg))
		return -1;

	if (!lock_vol(vg->cmd, VG_ORPHANS, LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return -1;
	}

	/* If device not initialized, pvcreate it */
	if (!pv_by_path(vg->cmd, device) &&
	   (!pvcreate_single(vg->cmd, device, NULL))) {
		log_error("Unable to initialize device for LVM use\n");
		unlock_vg(vg->cmd, VG_ORPHANS);
		return -1;
	}

	if (!vg_extend(vg, 1, (char **) &device)) {
		unlock_vg(vg->cmd, VG_ORPHANS);
		return -1;
	}
	/*
	 * FIXME: Either commit to disk, or keep holding VG_ORPHANS and
	 * release in lvm_vg_close().
	 */
	unlock_vg(vg->cmd, VG_ORPHANS);
	return 0;
}

int lvm_vg_set_extent_size(vg_t *vg, uint32_t new_size)
{
	if (vg_read_error(vg))
		return -1;

	if (!vg_set_extent_size(vg, new_size))
		return -1;
	return 0;
}

int lvm_vg_write(vg_t *vg)
{
	if (vg_read_error(vg))
		return -1;

	if (!archive(vg))
		return -1;

	/* Store VG on disk(s) */
	if (!vg_write(vg) || !vg_commit(vg))
		return -1;
	return 0;
}

int lvm_vg_close(vg_t *vg)
{
	if (vg_read_error(vg) == FAILED_LOCKING)
		vg_release(vg);
	else
		unlock_and_release_vg(vg->cmd, vg, vg->name);
	return 0;
}

int lvm_vg_remove(vg_t *vg)
{
	if (vg_read_error(vg))
		return -1;

	if (!vg_remove_single(vg))
		return -1;
	return 0;
}

vg_t *lvm_vg_open(lvm_t libh, const char *vgname, const char *mode,
		  uint32_t flags)
{
	uint32_t internal_flags = 0;
	vg_t *vg;

	if (!strncmp(mode, "w", 1))
		internal_flags |= READ_FOR_UPDATE;
	else if (strncmp(mode, "r", 1)) {
		log_errno(EINVAL, "Invalid VG open mode");
		return NULL;
	}

	vg = vg_read((struct cmd_context *)libh, vgname, NULL, internal_flags);
	if (vg_read_error(vg)) {
		/* FIXME: use log_errno either here in inside vg_read */
		vg_release(vg);
		return NULL;
	}

	return vg;
}

struct dm_list *lvm_vg_list_pvs(vg_t *vg)
{
	struct dm_list *list;
	pv_list_t *pvs;
	struct pv_list *pvl;

	if (dm_list_empty(&vg->pvs))
		return NULL;

	if (!(list = dm_pool_zalloc(vg->vgmem, sizeof(*list)))) {
		log_errno(ENOMEM, "Memory allocation fail for dm_list.\n");
		return NULL;
	}
	dm_list_init(list);

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(pvs = dm_pool_zalloc(vg->vgmem, sizeof(*pvs)))) {
			log_errno(ENOMEM,
				"Memory allocation fail for lvm_pv_list.\n");
			return NULL;
		}
		pvs->pv = pvl->pv;
		dm_list_add(list, &pvs->list);
	}
	return list;
}

struct dm_list *lvm_vg_list_lvs(vg_t *vg)
{
	struct dm_list *list;
	lv_list_t *lvs;
	struct lv_list *lvl;

	if (dm_list_empty(&vg->lvs))
		return NULL;

	if (!(list = dm_pool_zalloc(vg->vgmem, sizeof(*list)))) {
		log_errno(ENOMEM, "Memory allocation fail for dm_list.\n");
		return NULL;
	}
	dm_list_init(list);

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (!(lvs = dm_pool_zalloc(vg->vgmem, sizeof(*lvs)))) {
			log_errno(ENOMEM,
				"Memory allocation fail for lvm_lv_list.\n");
			return NULL;
		}
		lvs->lv = lvl->lv;
		dm_list_add(list, &lvs->list);
	}
	return list;
}

/* FIXME: invalid handle? return INTMAX? */
uint64_t lvm_vg_get_size(const vg_t *vg)
{
	return vg_size(vg);
}

uint64_t lvm_vg_get_free_size(const vg_t *vg)
{
	return vg_free(vg);
}

uint64_t lvm_vg_get_extent_size(const vg_t *vg)
{
	return vg_extent_size(vg);
}

uint64_t lvm_vg_get_extent_count(const vg_t *vg)
{
	return vg_extent_count(vg);
}

uint64_t lvm_vg_get_free_extent_count(const vg_t *vg)
{
	return vg_free_count(vg);
}

uint64_t lvm_vg_get_pv_count(const vg_t *vg)
{
	return vg_pv_count(vg);
}

char *lvm_vg_get_uuid(const vg_t *vg)
{
	char uuid[64] __attribute((aligned(8)));

	if (!id_write_format(&vg->id, uuid, sizeof(uuid))) {
		log_error("Internal error converting uuid");
		return NULL;
	}
	return strndup((const char *)uuid, 64);
}

char *lvm_vg_get_name(const vg_t *vg)
{
	char *name;

	name = malloc(NAME_LEN + 1);
	strncpy(name, (const char *)vg->name, NAME_LEN);
	name[NAME_LEN] = '\0';
	return name;
}

struct dm_list *lvm_list_vg_names(lvm_t libh)
{
	return get_vgnames((struct cmd_context *)libh, 0);
}

struct dm_list *lvm_list_vg_uuids(lvm_t libh)
{
	return get_vgids((struct cmd_context *)libh, 0);
}

int lvm_scan(lvm_t libh)
{
	if (!lvmcache_label_scan((struct cmd_context *)libh, 2))
		return -1;
	return 0;
}
