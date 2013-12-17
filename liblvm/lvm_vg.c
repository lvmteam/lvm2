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
#include "toolcontext.h"
#include "metadata.h"
#include "archiver.h"
#include "locking.h"
#include "lvmcache.h"
#include "lvmetad.h"
#include "lvm_misc.h"
#include "lvm2app.h"
#include "display.h"

int lvm_vg_add_tag(vg_t vg, const char *tag)
{
	int rc = -1;
	struct saved_env e = store_user_env(vg->cmd);

	if (!vg_read_error(vg) && vg_check_write_mode(vg) &&
		vg_change_tag(vg, tag, 1))
		rc = 0;
	restore_user_env(&e);
	return rc;
}


int lvm_vg_remove_tag(vg_t vg, const char *tag)
{
	int rc = -1;
	struct saved_env e = store_user_env(vg->cmd);

	if (!vg_read_error(vg) && vg_check_write_mode(vg) &&
		vg_change_tag(vg, tag, 0))
		rc = 0;
	restore_user_env(&e);
	return rc;
}


vg_t lvm_vg_create(lvm_t libh, const char *vg_name)
{
	struct volume_group *vg = NULL;
	struct saved_env e = store_user_env((struct cmd_context *)libh);

	vg = vg_create((struct cmd_context *)libh, vg_name);
	/* FIXME: error handling is still TBD */
	if (vg_read_error(vg)) {
		release_vg(vg);
		vg = NULL;
	} else {
		vg->open_mode = 'w';
	}

	restore_user_env(&e);
	return (vg_t) vg;
}

static int _lvm_vg_extend(vg_t vg, const char *device)
{
	struct pvcreate_params pp;

	if (vg_read_error(vg))
		return -1;

	if (!vg_check_write_mode(vg))
		return -1;

	if (!lock_vol(vg->cmd, VG_ORPHANS, LCK_VG_WRITE, NULL)) {
		log_error("Can't get lock for orphan PVs");
		return -1;
	}

	pvcreate_params_set_defaults(&pp);
	if (!vg_extend(vg, 1, &device, &pp)) {
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

int lvm_vg_extend(vg_t vg, const char *device)
{
	int rc = 0;
	struct saved_env e = store_user_env(vg->cmd);
	rc = _lvm_vg_extend(vg, device);
	restore_user_env(&e);
	return rc;
}

int lvm_vg_reduce(vg_t vg, const char *device)
{
	int rc = -1;
	struct saved_env e = store_user_env(vg->cmd);

	if (!vg_read_error(vg) && vg_check_write_mode(vg) && vg_reduce(vg, device))
		rc = 0;

	restore_user_env(&e);
	return rc;
}

int lvm_vg_set_extent_size(vg_t vg, uint32_t new_size)
{
	int rc = -1;
	struct saved_env e = store_user_env(vg->cmd);

	if (!vg_read_error(vg) && vg_check_write_mode(vg) &&
		vg_set_extent_size(vg, new_size / SECTOR_SIZE))
		rc = 0;

	restore_user_env(&e);
	return rc;
}

static int _lvm_vg_write(vg_t vg)
{
	struct pv_list *pvl;

	if (vg_read_error(vg))
		return -1;
	if (!vg_check_write_mode(vg))
		return -1;

	if (dm_list_empty(&vg->pvs)) {
		if (!vg_remove(vg))
			return -1;
		return 0;
	}

	if (! dm_list_empty(&vg->removed_pvs)) {
		if (!lock_vol(vg->cmd, VG_ORPHANS, LCK_VG_WRITE, NULL)) {
			log_error("Can't get lock for orphan PVs");
			return 0;
		}
	}

	if (!archive(vg))
		return -1;

	/* Store VG on disk(s) */
	if (!vg_write(vg) || !vg_commit(vg))
		return -1;

	if (! dm_list_empty(&vg->removed_pvs)) {
		dm_list_iterate_items(pvl, &vg->removed_pvs) {
			pv_write_orphan(vg->cmd, pvl->pv);
			pv_set_fid(pvl->pv, NULL);
			/* FIXME: do pvremove / label_remove()? */
		}
		dm_list_init(&vg->removed_pvs);
		unlock_vg(vg->cmd, VG_ORPHANS);
	}

	return 0;
}

int lvm_vg_write(vg_t vg)
{
	int rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = _lvm_vg_write(vg);
	restore_user_env(&e);
	return rc;
}

int lvm_vg_close(vg_t vg)
{
	struct saved_env e = store_user_env(vg->cmd);
	if (vg_read_error(vg) == FAILED_LOCKING)
		release_vg(vg);
	else
		unlock_and_release_vg(vg->cmd, vg, vg->name);
	restore_user_env(&e);
	return 0;
}

int lvm_vg_remove(vg_t vg)
{
	int rc = -1;
	struct saved_env e = store_user_env(vg->cmd);

	if (!vg_read_error(vg) && vg_check_write_mode(vg) && vg_remove_check(vg)) {
		vg_remove_pvs(vg);
		rc = 0;
	}

	restore_user_env(&e);
	return rc;
}

static vg_t _lvm_vg_open(lvm_t libh, const char *vgname, const char *mode,
		  uint32_t flags)
{
	uint32_t internal_flags = 0;
	struct volume_group *vg;

	if (!strncmp(mode, "w", 1))
		internal_flags |= READ_FOR_UPDATE;
	else if (strncmp(mode, "r", 1)) {
		log_errno(EINVAL, "Invalid VG open mode");
		return NULL;
	}

	vg = vg_read((struct cmd_context *)libh, vgname, NULL, internal_flags);
	if (vg_read_error(vg)) {
		/* FIXME: use log_errno either here in inside vg_read */
		release_vg(vg);
		return NULL;
	}
	/* FIXME: combine this with locking ? */
	vg->open_mode = mode[0];

	return (vg_t) vg;
}

vg_t lvm_vg_open(lvm_t libh, const char *vgname, const char *mode,
		  uint32_t flags)
{
	vg_t rc;
	struct saved_env e = store_user_env((struct cmd_context*)libh);
	rc = _lvm_vg_open(libh, vgname, mode, flags);
	restore_user_env(&e);
	return rc;
}

static struct dm_list *_lvm_vg_list_pvs(vg_t vg)
{
	struct dm_list *list;
	pv_list_t *pvs;
	struct pv_list *pvl;

	if (dm_list_empty(&vg->pvs))
		return NULL;

	if (!(list = dm_pool_zalloc(vg->vgmem, sizeof(*list)))) {
		log_errno(ENOMEM, "Memory allocation fail for dm_list.");
		return NULL;
	}
	dm_list_init(list);

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(pvs = dm_pool_zalloc(vg->vgmem, sizeof(*pvs)))) {
			log_errno(ENOMEM,
				"Memory allocation fail for lvm_pv_list.");
			return NULL;
		}
		pvs->pv = pvl->pv;
		dm_list_add(list, &pvs->list);
	}
	return list;
}

struct dm_list *lvm_vg_list_pvs(vg_t vg)
{
	struct dm_list *rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = _lvm_vg_list_pvs(vg);
	restore_user_env(&e);
	return rc;
}

static struct dm_list *_lvm_vg_list_lvs(vg_t vg)
{
	struct dm_list *list;
	lv_list_t *lvs;
	struct lv_list *lvl;

	if (dm_list_empty(&vg->lvs))
		return NULL;

	if (!(list = dm_pool_zalloc(vg->vgmem, sizeof(*list)))) {
		log_errno(ENOMEM, "Memory allocation fail for dm_list.");
		return NULL;
	}
	dm_list_init(list);

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (!(lvs = dm_pool_zalloc(vg->vgmem, sizeof(*lvs)))) {
			log_errno(ENOMEM,
				"Memory allocation fail for lvm_lv_list.");
			return NULL;
		}
		lvs->lv = lvl->lv;
		dm_list_add(list, &lvs->list);
	}
	return list;
}

struct dm_list *lvm_vg_list_lvs(vg_t vg)
{
	struct dm_list *rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = _lvm_vg_list_lvs(vg);
	restore_user_env(&e);
	return rc;
}

struct dm_list *lvm_vg_get_tags(const vg_t vg)
{
	struct dm_list *rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = tag_list_copy(vg->vgmem, &vg->tags);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_get_seqno(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_seqno(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_is_clustered(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_is_clustered(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_is_exported(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_is_exported(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_is_partial(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = (vg_missing_pv_count(vg) != 0);
	restore_user_env(&e);
	return rc;
}

/* FIXME: invalid handle? return INTMAX? */
uint64_t lvm_vg_get_size(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = SECTOR_SIZE * vg_size(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_get_free_size(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = SECTOR_SIZE * vg_free(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_get_extent_size(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = SECTOR_SIZE * vg_extent_size(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_get_extent_count(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_extent_count(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_get_free_extent_count(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_free_count(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_get_pv_count(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_pv_count(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_get_max_pv(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_max_pv(vg);
	restore_user_env(&e);
	return rc;
}

uint64_t lvm_vg_get_max_lv(const vg_t vg)
{
	uint64_t rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_max_lv(vg);
	restore_user_env(&e);
	return rc;
}

const char *lvm_vg_get_uuid(const vg_t vg)
{
	const char *rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = vg_uuid_dup(vg);
	restore_user_env(&e);
	return rc;
}

const char *lvm_vg_get_name(const vg_t vg)
{
	const char *rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = dm_pool_strndup(vg->vgmem, (const char *)vg->name, NAME_LEN+1);
	restore_user_env(&e);
	return rc;
}


struct lvm_property_value lvm_vg_get_property(const vg_t vg, const char *name)
{
	struct lvm_property_value rc;
	struct saved_env e = store_user_env(vg->cmd);
	rc = get_property(NULL, vg, NULL, NULL, NULL, NULL, NULL, name);
	restore_user_env(&e);
	return rc;
}

int lvm_vg_set_property(const vg_t vg, const char *name,
			struct lvm_property_value *value)
{
	/* At this point it is unknown if all property set paths make the
	 * appropriate copy of the string.  We will allocate a copy on the vg so
	 * that worst case we have two copies which will get freed when the vg gets
	 * released.
	 */
	int rc;
	struct saved_env e = store_user_env(vg->cmd);

	if (value->is_valid && value->is_string && value->value.string) {
		value->value.string = dm_pool_strndup(vg->vgmem, value->value.string,
				strlen(value->value.string) + 1);
	}

	rc = set_property(NULL, vg, NULL, NULL, NULL, name, value);
	restore_user_env(&e);
	return rc;
}

struct dm_list *lvm_list_vg_names(lvm_t libh)
{
	struct dm_list *rc = NULL;
	struct saved_env e = store_user_env((struct cmd_context *)libh);

	if (lvmetad_vg_list_to_lvmcache((struct cmd_context *)libh)) {
		rc = get_vgnames((struct cmd_context *)libh, 0);
	}
	restore_user_env(&e);
	return rc;
}

struct dm_list *lvm_list_vg_uuids(lvm_t libh)
{
	struct dm_list *rc = NULL;
	struct saved_env e = store_user_env((struct cmd_context *)libh);

	if (lvmetad_vg_list_to_lvmcache((struct cmd_context *)libh)) {
		rc = get_vgids((struct cmd_context *)libh, 0);
	}
	restore_user_env(&e);
	return rc;
}

/*
 * FIXME: Elaborate on when to use, side-effects, .cache file, etc
 */
int lvm_scan(lvm_t libh)
{
	int rc = 0;
	struct saved_env e = store_user_env((struct cmd_context *)libh);

	if (!lvmcache_label_scan((struct cmd_context *)libh, 2))
		rc = -1;

	restore_user_env(&e);
	return rc;
}

int lvm_lv_name_validate(const vg_t vg, const char *name)
{
	int rc = -1;
	name_error_t name_error;

	struct saved_env e = store_user_env(vg->cmd);

	name_error = validate_name_detailed(name);

	if (NAME_VALID == name_error) {
		if (apply_lvname_restrictions(name)) {
			if (!find_lv_in_vg(vg, name)) {
				rc = 0;
			} else {
				log_errno(EINVAL, "LV name exists in VG");
			}
		}
	} else {
		display_name_error(name_error);
	}

	restore_user_env(&e);
	return rc;
}

int lvm_vg_name_validate(lvm_t libh, const char *name)
{
	int rc = -1;
	struct cmd_context *cmd = (struct cmd_context *)libh;
	struct saved_env e = store_user_env(cmd);

	if (validate_new_vg_name(cmd, name))
		rc = 0;

	restore_user_env(&e);
	return rc;
}
