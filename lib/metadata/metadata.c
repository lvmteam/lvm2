/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
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
#include "device.h"
#include "metadata.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "lvm-file.h"
#include "lvmcache.h"
#include "memlock.h"
#include "str_list.h"
#include "pv_alloc.h"
#include "segtype.h"
#include "activate.h"
#include "display.h"
#include "locking.h"
#include "archiver.h"
#include "defaults.h"
#include "filter-persistent.h"

#include <math.h>
#include <sys/param.h>

static struct physical_volume *_pv_read(struct cmd_context *cmd,
					struct dm_pool *pvmem,
					const char *pv_name,
					struct format_instance *fid,
					int warnings, int scan_label_only);

static struct physical_volume *_find_pv_by_name(struct cmd_context *cmd,
			 			const char *pv_name);

static struct pv_list *_find_pv_in_vg(const struct volume_group *vg,
				      const char *pv_name);

static struct pv_list *_find_pv_in_vg_by_uuid(const struct volume_group *vg,
					      const struct id *id);

static uint32_t _vg_bad_status_bits(const struct volume_group *vg,
				    uint64_t status);

const char _really_init[] =
    "Really INITIALIZE physical volume \"%s\" of volume group \"%s\" [y/n]? ";

static int _alignment_overrides_default(unsigned long data_alignment,
					unsigned long default_pe_align)
{
	return data_alignment && (default_pe_align % data_alignment);
}

unsigned long set_pe_align(struct physical_volume *pv, unsigned long data_alignment)
{
	unsigned long default_pe_align, temp_pe_align;

	if (pv->pe_align)
		goto out;

	if (data_alignment) {
		/* Always use specified data_alignment */
		pv->pe_align = data_alignment;
		goto out;
	}

	default_pe_align = find_config_tree_int(pv->fmt->cmd,
						"devices/default_data_alignment",
						DEFAULT_DATA_ALIGNMENT);

	if (default_pe_align)
		/* align on 1 MiB multiple */
		default_pe_align *= DEFAULT_PE_ALIGN;
	else
		/* align on 64 KiB multiple (old default) */
		default_pe_align = DEFAULT_PE_ALIGN_OLD;

	pv->pe_align = MAX((default_pe_align << SECTOR_SHIFT),
			   lvm_getpagesize()) >> SECTOR_SHIFT;

	if (!pv->dev)
		goto out;

	/*
	 * Align to stripe-width of underlying md device if present
	 */
	if (find_config_tree_bool(pv->fmt->cmd, "devices/md_chunk_alignment",
				  DEFAULT_MD_CHUNK_ALIGNMENT)) {
		temp_pe_align = dev_md_stripe_width(pv->fmt->cmd->sysfs_dir, pv->dev);
		if (_alignment_overrides_default(temp_pe_align, default_pe_align))
			pv->pe_align = MAX(pv->pe_align, temp_pe_align);
	}

	/*
	 * Align to topology's minimum_io_size or optimal_io_size if present
	 * - minimum_io_size - the smallest request the device can perform
	 *   w/o incurring a read-modify-write penalty (e.g. MD's chunk size)
	 * - optimal_io_size - the device's preferred unit of receiving I/O
	 *   (e.g. MD's stripe width)
	 */
	if (find_config_tree_bool(pv->fmt->cmd,
				  "devices/data_alignment_detection",
				  DEFAULT_DATA_ALIGNMENT_DETECTION)) {
		temp_pe_align = dev_minimum_io_size(pv->fmt->cmd->sysfs_dir, pv->dev);
		if (_alignment_overrides_default(temp_pe_align, default_pe_align))
			pv->pe_align = MAX(pv->pe_align, temp_pe_align);

		temp_pe_align = dev_optimal_io_size(pv->fmt->cmd->sysfs_dir, pv->dev);
		if (_alignment_overrides_default(temp_pe_align, default_pe_align))
			pv->pe_align = MAX(pv->pe_align, temp_pe_align);
	}

out:
	log_very_verbose("%s: Setting PE alignment to %lu sectors.",
			 dev_name(pv->dev), pv->pe_align);

	return pv->pe_align;
}

unsigned long set_pe_align_offset(struct physical_volume *pv,
				  unsigned long data_alignment_offset)
{
	if (pv->pe_align_offset)
		goto out;

	if (data_alignment_offset) {
		/* Always use specified data_alignment_offset */
		pv->pe_align_offset = data_alignment_offset;
		goto out;
	}

	if (!pv->dev)
		goto out;

	if (find_config_tree_bool(pv->fmt->cmd,
				  "devices/data_alignment_offset_detection",
				  DEFAULT_DATA_ALIGNMENT_OFFSET_DETECTION)) {
		int align_offset = dev_alignment_offset(pv->fmt->cmd->sysfs_dir,
							pv->dev);
		/* must handle a -1 alignment_offset; means dev is misaligned */
		if (align_offset < 0)
			align_offset = 0;
		pv->pe_align_offset = MAX(pv->pe_align_offset, align_offset);
	}

out:
	log_very_verbose("%s: Setting PE alignment offset to %lu sectors.",
			 dev_name(pv->dev), pv->pe_align_offset);

	return pv->pe_align_offset;
}

void add_pvl_to_vgs(struct volume_group *vg, struct pv_list *pvl)
{
	dm_list_add(&vg->pvs, &pvl->list);
	vg->pv_count++;
	pvl->pv->vg = vg;
	pv_set_fid(pvl->pv, vg->fid);
}

void del_pvl_from_vgs(struct volume_group *vg, struct pv_list *pvl)
{
	struct lvmcache_info *info;

	vg->pv_count--;
	dm_list_del(&pvl->list);

	pvl->pv->vg = vg->fid->fmt->orphan_vg; /* orphan */
	if ((info = lvmcache_info_from_pvid((const char *) &pvl->pv->id, 0)))
		lvmcache_fid_add_mdas(info, vg->fid->fmt->orphan_vg->fid,
				      (const char *) &pvl->pv->id, ID_LEN);
	pv_set_fid(pvl->pv, vg->fid->fmt->orphan_vg->fid);
}

/**
 * add_pv_to_vg - Add a physical volume to a volume group
 * @vg - volume group to add to
 * @pv_name - name of the pv (to be removed)
 * @pv - physical volume to add to volume group
 * @pp - physical volume creation params (OPTIONAL)
 *
 * Returns:
 *  0 - failure
 *  1 - success
 * FIXME: remove pv_name - obtain safely from pv
 */
int add_pv_to_vg(struct volume_group *vg, const char *pv_name,
		 struct physical_volume *pv, struct pvcreate_params *pp)
{
	struct pv_to_create *pvc;
	struct pv_list *pvl;
	struct format_instance *fid = vg->fid;
	struct dm_pool *mem = vg->vgmem;
	char uuid[64] __attribute__((aligned(8)));

	log_verbose("Adding physical volume '%s' to volume group '%s'",
		    pv_name, vg->name);

	if (!(pvl = dm_pool_zalloc(mem, sizeof(*pvl)))) {
		log_error("pv_list allocation for '%s' failed", pv_name);
		return 0;
	}

	if (!is_orphan_vg(pv->vg_name)) {
		log_error("Physical volume '%s' is already in volume group "
			  "'%s'", pv_name, pv->vg_name);
		return 0;
	}

	if (pv->fmt != fid->fmt) {
		log_error("Physical volume %s is of different format type (%s)",
			  pv_name, pv->fmt->name);
		return 0;
	}

	/* Ensure PV doesn't depend on another PV already in the VG */
	if (pv_uses_vg(pv, vg)) {
		log_error("Physical volume %s might be constructed from same "
			  "volume group %s", pv_name, vg->name);
		return 0;
	}

	if (!(pv->vg_name = dm_pool_strdup(mem, vg->name))) {
		log_error("vg->name allocation failed for '%s'", pv_name);
		return 0;
	}

	memcpy(&pv->vgid, &vg->id, sizeof(vg->id));

	/* Units of 512-byte sectors */
	pv->pe_size = vg->extent_size;

	/*
	 * pe_count must always be calculated by pv_setup
	 */
	pv->pe_alloc_count = 0;

	if (!fid->fmt->ops->pv_setup(fid->fmt, pv, vg)) {
		log_error("Format-specific setup of physical volume '%s' "
			  "failed.", pv_name);
		return 0;
	}

	if (_find_pv_in_vg(vg, pv_name) ||
	    _find_pv_in_vg_by_uuid(vg, &pv->id)) {
		if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
			stack;
			uuid[0] = '\0';
		}
		log_error("Physical volume '%s (%s)' listed more than once.",
			  pv_name, uuid);
		return 0;
	}

	if (vg->pv_count && (vg->pv_count == vg->max_pv)) {
		log_error("No space for '%s' - volume group '%s' "
			  "holds max %d physical volume(s).", pv_name,
			  vg->name, vg->max_pv);
		return 0;
	}

	if (!alloc_pv_segment_whole_pv(mem, pv))
		return_0;

	if ((uint64_t) vg->extent_count + pv->pe_count > MAX_EXTENT_COUNT) {
		log_error("Unable to add %s to %s: new extent count (%"
			  PRIu64 ") exceeds limit (%" PRIu32 ").",
			  pv_name, vg->name,
			  (uint64_t) vg->extent_count + pv->pe_count,
			  MAX_EXTENT_COUNT);
		return 0;
	}

	pvl->pv = pv;
	add_pvl_to_vgs(vg, pvl);
	vg->extent_count += pv->pe_count;
	vg->free_count += pv->pe_count;

	if (pv->status & UNLABELLED_PV) {
		if (!(pvc = dm_pool_zalloc(mem, sizeof(*pvc)))) {
			log_error("pv_to_create allocation for '%s' failed", pv_name);
			return 0;
		}
		pvc->pv = pv;
		pvc->pp = pp;
		dm_list_add(&vg->pvs_to_create, &pvc->list);
	}

	return 1;
}

static int _copy_pv(struct dm_pool *pvmem,
		    struct physical_volume *pv_to,
		    struct physical_volume *pv_from)
{
	memcpy(pv_to, pv_from, sizeof(*pv_to));

	/* We must use pv_set_fid here to update the reference counter! */
	pv_to->fid = NULL;
	pv_set_fid(pv_to, pv_from->fid);

	if (!(pv_to->vg_name = dm_pool_strdup(pvmem, pv_from->vg_name)))
		return_0;

	if (!str_list_dup(pvmem, &pv_to->tags, &pv_from->tags))
		return_0;

	if (!peg_dup(pvmem, &pv_to->segments, &pv_from->segments))
		return_0;

	return 1;
}

static struct pv_list *_copy_pvl(struct dm_pool *pvmem, struct pv_list *pvl_from)
{
	struct pv_list *pvl_to = NULL;

	if (!(pvl_to = dm_pool_zalloc(pvmem, sizeof(*pvl_to))))
		return_NULL;

	if (!(pvl_to->pv = dm_pool_alloc(pvmem, sizeof(*pvl_to->pv))))
		goto_bad;

	if(!_copy_pv(pvmem, pvl_to->pv, pvl_from->pv))
		goto_bad;

	return pvl_to;
bad:
	dm_pool_free(pvmem, pvl_to);
	return NULL;
}

int get_pv_from_vg_by_id(const struct format_type *fmt, const char *vg_name,
			 const char *vgid, const char *pvid,
			 struct physical_volume *pv)
{
	struct volume_group *vg;
	struct pv_list *pvl;
	int r = 0, consistent = 0;

	if (!(vg = vg_read_internal(fmt->cmd, vg_name, vgid, 1, &consistent))) {
		log_error("get_pv_from_vg_by_id: vg_read_internal failed to read VG %s",
			  vg_name);
		return 0;
	}

	if (!consistent)
		log_warn("WARNING: Volume group %s is not consistent",
			 vg_name);

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (id_equal(&pvl->pv->id, (const struct id *) pvid)) {
			if (!_copy_pv(fmt->cmd->mem, pv, pvl->pv)) {
				log_error("internal PV duplication failed");
				r = 0;
				goto out;
			}
			r = 1;
			goto out;
		}
	}
out:
	release_vg(vg);
	return r;
}

int move_pv(struct volume_group *vg_from, struct volume_group *vg_to,
	    const char *pv_name)
{
	struct physical_volume *pv;
	struct pv_list *pvl;

	/* FIXME: handle tags */
	if (!(pvl = find_pv_in_vg(vg_from, pv_name))) {
		log_error("Physical volume %s not in volume group %s",
			  pv_name, vg_from->name);
		return 0;
	}

	if (_vg_bad_status_bits(vg_from, RESIZEABLE_VG) ||
	    _vg_bad_status_bits(vg_to, RESIZEABLE_VG))
		return 0;

	del_pvl_from_vgs(vg_from, pvl);
	add_pvl_to_vgs(vg_to, pvl);

	pv = pvl->pv;

	vg_from->extent_count -= pv_pe_count(pv);
	vg_to->extent_count += pv_pe_count(pv);

	vg_from->free_count -= pv_pe_count(pv) - pv_pe_alloc_count(pv);
	vg_to->free_count += pv_pe_count(pv) - pv_pe_alloc_count(pv);

	return 1;
}

int move_pvs_used_by_lv(struct volume_group *vg_from,
			struct volume_group *vg_to,
			const char *lv_name)
{
	struct lv_segment *lvseg;
	unsigned s;
	struct lv_list *lvl;
	struct logical_volume *lv;

	/* FIXME: handle tags */
	if (!(lvl = find_lv_in_vg(vg_from, lv_name))) {
		log_error("Logical volume %s not in volume group %s",
			  lv_name, vg_from->name);
		return 0;
	}

	if (_vg_bad_status_bits(vg_from, RESIZEABLE_VG) ||
	    _vg_bad_status_bits(vg_to, RESIZEABLE_VG))
		return 0;

	dm_list_iterate_items(lvseg, &lvl->lv->segments) {
		if (lvseg->log_lv)
			if (!move_pvs_used_by_lv(vg_from, vg_to,
						     lvseg->log_lv->name))
				return_0;
		for (s = 0; s < lvseg->area_count; s++) {
			if (seg_type(lvseg, s) == AREA_PV) {
				if (!move_pv(vg_from, vg_to,
					      pv_dev_name(seg_pv(lvseg, s))))
					return_0;
			} else if (seg_type(lvseg, s) == AREA_LV) {
				lv = seg_lv(lvseg, s);
				if (!move_pvs_used_by_lv(vg_from, vg_to,
							     lv->name))
				    return_0;
			}
		}
	}
	return 1;
}

static int validate_new_vg_name(struct cmd_context *cmd, const char *vg_name)
{
	static char vg_path[PATH_MAX];

	if (!validate_name(vg_name))
		return_0;

	snprintf(vg_path, sizeof(vg_path), "%s%s", cmd->dev_dir, vg_name);
	if (path_exists(vg_path)) {
		log_error("%s: already exists in filesystem", vg_path);
		return 0;
	}

	return 1;
}

int validate_vg_rename_params(struct cmd_context *cmd,
			      const char *vg_name_old,
			      const char *vg_name_new)
{
	unsigned length;
	char *dev_dir;

	dev_dir = cmd->dev_dir;
	length = strlen(dev_dir);

	/* Check sanity of new name */
	if (strlen(vg_name_new) > NAME_LEN - length - 2) {
		log_error("New volume group path exceeds maximum length "
			  "of %d!", NAME_LEN - length - 2);
		return 0;
	}

	if (!validate_new_vg_name(cmd, vg_name_new)) {
		log_error("New volume group name \"%s\" is invalid",
			  vg_name_new);
		return 0;
	}

	if (!strcmp(vg_name_old, vg_name_new)) {
		log_error("Old and new volume group names must differ");
		return 0;
	}

	return 1;
}

int vg_rename(struct cmd_context *cmd, struct volume_group *vg,
	      const char *new_name)
{
	struct dm_pool *mem = vg->vgmem;
	struct pv_list *pvl;

	vg->old_name = vg->name;

	if (!(vg->name = dm_pool_strdup(mem, new_name))) {
		log_error("vg->name allocation failed for '%s'", new_name);
		return 0;
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(pvl->pv->vg_name = dm_pool_strdup(mem, new_name))) {
			log_error("pv->vg_name allocation failed for '%s'",
				  pv_dev_name(pvl->pv));
			return 0;
		}
	}

	return 1;
}

int remove_lvs_in_vg(struct cmd_context *cmd,
		     struct volume_group *vg,
		     force_t force)
{
	struct dm_list *lst;
	struct lv_list *lvl;

	while ((lst = dm_list_first(&vg->lvs))) {
		lvl = dm_list_item(lst, struct lv_list);
		if (!lv_remove_with_dependencies(cmd, lvl->lv, force, 0))
		    return 0;
	}

	return 1;
}

int vg_remove_check(struct volume_group *vg)
{
	unsigned lv_count;

	if (vg_read_error(vg) || vg_missing_pv_count(vg)) {
		log_error("Volume group \"%s\" not found, is inconsistent "
			  "or has PVs missing.", vg ? vg->name : "");
		log_error("Consider vgreduce --removemissing if metadata "
			  "is inconsistent.");
		return 0;
	}

	if (!vg_check_status(vg, EXPORTED_VG))
		return 0;

	lv_count = vg_visible_lvs(vg);

	if (lv_count) {
		log_error("Volume group \"%s\" still contains %u "
			  "logical volume(s)", vg->name, lv_count);
		return 0;
	}

	if (!archive(vg))
		return 0;

	return 1;
}

void vg_remove_pvs(struct volume_group *vg)
{
	struct pv_list *pvl, *tpvl;

	dm_list_iterate_items_safe(pvl, tpvl, &vg->pvs) {
		del_pvl_from_vgs(vg, pvl);
		dm_list_add(&vg->removed_pvs, &pvl->list);
	}
}

int vg_remove(struct volume_group *vg)
{
	struct physical_volume *pv;
	struct pv_list *pvl;
	int ret = 1;

	if (!lock_vol(vg->cmd, VG_ORPHANS, LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return 0;
	}

	if (!vg_remove_mdas(vg)) {
		log_error("vg_remove_mdas %s failed", vg->name);
		unlock_vg(vg->cmd, VG_ORPHANS);
		return 0;
	}

	/* init physical volumes */
	dm_list_iterate_items(pvl, &vg->removed_pvs) {
		pv = pvl->pv;
		if (is_missing_pv(pv))
			continue;

		log_verbose("Removing physical volume \"%s\" from "
			    "volume group \"%s\"", pv_dev_name(pv), vg->name);
		pv->vg_name = vg->fid->fmt->orphan_vg_name;
		pv->status = ALLOCATABLE_PV;

		if (!dev_get_size(pv_dev(pv), &pv->size)) {
			log_error("%s: Couldn't get size.", pv_dev_name(pv));
			ret = 0;
			continue;
		}

		/* FIXME Write to same sector label was read from */
		if (!pv_write(vg->cmd, pv, 0)) {
			log_error("Failed to remove physical volume \"%s\""
				  " from volume group \"%s\"",
				  pv_dev_name(pv), vg->name);
			ret = 0;
		}
	}

	if (!backup_remove(vg->cmd, vg->name))
		stack;

	if (ret)
		log_print("Volume group \"%s\" successfully removed", vg->name);
	else
		log_error("Volume group \"%s\" not properly removed", vg->name);

	unlock_vg(vg->cmd, VG_ORPHANS);
	return ret;
}

/*
 * Extend a VG by a single PV / device path
 *
 * Parameters:
 * - vg: handle of volume group to extend by 'pv_name'
 * - pv_name: device path of PV to add to VG
 * - pp: parameters to pass to implicit pvcreate; if NULL, do not pvcreate
 *
 */
static int vg_extend_single_pv(struct volume_group *vg, char *pv_name,
			       struct pvcreate_params *pp)
{
	struct physical_volume *pv;

	pv = pv_by_path(vg->fid->fmt->cmd, pv_name);
	if (!pv && !pp) {
		log_error("%s not identified as an existing "
			  "physical volume", pv_name);
		return 0;
	} else if (!pv && pp) {
		pv = pvcreate_single(vg->cmd, pv_name, pp, 0);
		if (!pv)
			return 0;
	}
	if (!add_pv_to_vg(vg, pv_name, pv, pp)) {
		free_pv_fid(pv);
		return 0;
	}
	return 1;
}

/*
 * Extend a VG by a single PV / device path
 *
 * Parameters:
 * - vg: handle of volume group to extend by 'pv_name'
 * - pv_count: count of device paths of PVs
 * - pv_names: device paths of PVs to add to VG
 * - pp: parameters to pass to implicit pvcreate; if NULL, do not pvcreate
 *
 */
int vg_extend(struct volume_group *vg, int pv_count, const char *const *pv_names,
	      struct pvcreate_params *pp)
{
	int i;
	char *pv_name;

	if (_vg_bad_status_bits(vg, RESIZEABLE_VG))
		return 0;

	/* attach each pv */
	for (i = 0; i < pv_count; i++) {
		if (!(pv_name = dm_strdup(pv_names[i]))) {
			log_error("Failed to duplicate pv name %s.", pv_names[i]);
			return 0;
		}
		dm_unescape_colons_and_at_signs(pv_name, NULL, NULL);
		if (!vg_extend_single_pv(vg, pv_name, pp)) {
			log_error("Unable to add physical volume '%s' to "
				  "volume group '%s'.", pv_name, vg->name);
			dm_free(pv_name);
			return 0;
		}
		dm_free(pv_name);
	}

/* FIXME Decide whether to initialise and add new mdahs to format instance */

	return 1;
}

/* FIXME: use this inside vgreduce_single? */
int vg_reduce(struct volume_group *vg, const char *pv_name)
{
	struct physical_volume *pv;
	struct pv_list *pvl;

	if (_vg_bad_status_bits(vg, RESIZEABLE_VG))
		return 0;

	if (!archive(vg))
		goto bad;

	/* remove each pv */
	if (!(pvl = find_pv_in_vg(vg, pv_name))) {
		log_error("Physical volume %s not in volume group %s.",
			  pv_name, vg->name);
		goto bad;
	}

	pv = pvl->pv;

	if (pv_pe_alloc_count(pv)) {
		log_error("Physical volume %s still in use.",
			  pv_name);
		goto bad;
	}

	if (!dev_get_size(pv_dev(pv), &pv->size)) {
		log_error("%s: Couldn't get size.", pv_name);
		goto bad;
	}

	vg->free_count -= pv_pe_count(pv) - pv_pe_alloc_count(pv);
	vg->extent_count -= pv_pe_count(pv);
	del_pvl_from_vgs(vg, pvl);

	/* add pv to the remove_pvs list */
	dm_list_add(&vg->removed_pvs, &pvl->list);

	return 1;

      bad:
	log_error("Unable to remove physical volume '%s' from "
		  "volume group '%s'.", pv_name, vg->name);
	return 0;
}

int lv_change_tag(struct logical_volume *lv, const char *tag, int add_tag)
{
	char *tag_new;

	if (!(lv->vg->fid->fmt->features & FMT_TAGS)) {
		log_error("Logical volume %s/%s does not support tags",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (add_tag) {
		if (!(tag_new = dm_pool_strdup(lv->vg->vgmem, tag))) {
			log_error("Failed to duplicate tag %s from %s/%s",
				  tag, lv->vg->name, lv->name);
			return 0;
		}
		if (!str_list_add(lv->vg->vgmem, &lv->tags, tag_new)) {
			log_error("Failed to add tag %s to %s/%s",
				  tag, lv->vg->name, lv->name);
			return 0;
		}
	} else
		str_list_del(&lv->tags, tag);

	return 1;
}

int vg_change_tag(struct volume_group *vg, const char *tag, int add_tag)
{
	char *tag_new;

	if (!(vg->fid->fmt->features & FMT_TAGS)) {
		log_error("Volume group %s does not support tags", vg->name);
		return 0;
	}

	if (add_tag) {
		if (!(tag_new = dm_pool_strdup(vg->vgmem, tag))) {
			log_error("Failed to duplicate tag %s from %s",
				  tag, vg->name);
			return 0;
		}
		if (!str_list_add(vg->vgmem, &vg->tags, tag_new)) {
			log_error("Failed to add tag %s to volume group %s",
				  tag, vg->name);
			return 0;
		}
	} else
		str_list_del(&vg->tags, tag);

	return 1;
}

const char *strip_dir(const char *vg_name, const char *dev_dir)
{
	size_t len = strlen(dev_dir);
	if (!strncmp(vg_name, dev_dir, len))
		vg_name += len;

	return vg_name;
}

/*
 * Validate parameters to vg_create() before calling.
 * FIXME: Move inside vg_create library function.
 * FIXME: Change vgcreate_params struct to individual gets/sets
 */
int vgcreate_params_validate(struct cmd_context *cmd,
			     struct vgcreate_params *vp)
{
	if (!validate_new_vg_name(cmd, vp->vg_name)) {
		log_error("New volume group name \"%s\" is invalid",
			  vp->vg_name);
		return 1;
	}

	if (vp->alloc == ALLOC_INHERIT) {
		log_error("Volume Group allocation policy cannot inherit "
			  "from anything");
		return 1;
	}

	if (!vp->extent_size) {
		log_error("Physical extent size may not be zero");
		return 1;
	}

	if (!(cmd->fmt->features & FMT_UNLIMITED_VOLS)) {
		if (!vp->max_lv)
			vp->max_lv = 255;
		if (!vp->max_pv)
			vp->max_pv = 255;
		if (vp->max_lv > 255 || vp->max_pv > 255) {
			log_error("Number of volumes may not exceed 255");
			return 1;
		}
	}

	return 0;
}

/*
 * Create a (struct volume_group) volume group handle from a struct volume_group pointer and a
 * possible failure code or zero for success.
 */
static struct volume_group *_vg_make_handle(struct cmd_context *cmd,
					    struct volume_group *vg,
					    uint32_t failure)
{
	/* Never return a cached VG structure for a failure */
	if (vg && vg->vginfo && failure != SUCCESS) {
		release_vg(vg);
		vg = NULL;
	}

	if (!vg && !(vg = alloc_vg("vg_make_handle", cmd, NULL)))
		return_NULL;

	if (vg->read_status != failure)
		vg->read_status = failure;

	return vg;
}

int lv_has_unknown_segments(const struct logical_volume *lv)
{
	struct lv_segment *seg;
	/* foreach segment */
	dm_list_iterate_items(seg, &lv->segments)
		if (seg_unknown(seg))
			return 1;
	return 0;
}

int vg_has_unknown_segments(const struct volume_group *vg)
{
	struct lv_list *lvl;

	/* foreach LV */
	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_has_unknown_segments(lvl->lv))
			return 1;
	return 0;
}

/*
 * Create a VG with default parameters.
 * Returns:
 * - struct volume_group* with SUCCESS code: VG structure created
 * - NULL or struct volume_group* with FAILED_* code: error creating VG structure
 * Use vg_read_error() to determine success or failure.
 * FIXME: cleanup usage of _vg_make_handle()
 */
struct volume_group *vg_create(struct cmd_context *cmd, const char *vg_name)
{
	struct volume_group *vg;
	struct format_instance_ctx fic;
	struct format_instance *fid;
	int consistent = 0;
	uint32_t rc;

	if (!validate_name(vg_name)) {
		log_error("Invalid vg name %s", vg_name);
		/* FIXME: use _vg_make_handle() w/proper error code */
		return NULL;
	}

	rc = vg_lock_newname(cmd, vg_name);
	if (rc != SUCCESS)
		/* NOTE: let caller decide - this may be check for existence */
		return _vg_make_handle(cmd, NULL, rc);

	/* FIXME: Is this vg_read_internal necessary? Move it inside
	   vg_lock_newname? */
	/* is this vg name already in use ? */
	if ((vg = vg_read_internal(cmd, vg_name, NULL, 1, &consistent))) {
		log_error("A volume group called '%s' already exists.", vg_name);
		unlock_and_release_vg(cmd, vg, vg_name);
		return _vg_make_handle(cmd, NULL, FAILED_EXIST);
	}

	/* Strip dev_dir if present */
	vg_name = strip_dir(vg_name, cmd->dev_dir);

	if (!(vg = alloc_vg("vg_create", cmd, vg_name)))
		goto_bad;

	if (!id_create(&vg->id)) {
		log_error("Couldn't create uuid for volume group '%s'.",
			  vg_name);
		goto bad;
	}

	vg->status = (RESIZEABLE_VG | LVM_READ | LVM_WRITE);
	if (!(vg->system_id = dm_pool_zalloc(vg->vgmem, NAME_LEN + 1)))
		goto_bad;

	*vg->system_id = '\0';

	vg->extent_size = DEFAULT_EXTENT_SIZE * 2;
	vg->extent_count = 0;
	vg->free_count = 0;

	vg->max_lv = DEFAULT_MAX_LV;
	vg->max_pv = DEFAULT_MAX_PV;

	vg->alloc = DEFAULT_ALLOC_POLICY;
	vg->mda_copies = DEFAULT_VGMETADATACOPIES;

	vg->pv_count = 0;

	fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = vg_name;
	fic.context.vg_ref.vg_id = NULL;
	if (!(fid = cmd->fmt->ops->create_instance(cmd->fmt, &fic))) {
		log_error("Failed to create format instance");
		goto bad;
	}
	vg_set_fid(vg, fid);

	if (vg->fid->fmt->ops->vg_setup &&
	    !vg->fid->fmt->ops->vg_setup(vg->fid, vg)) {
		log_error("Format specific setup of volume group '%s' failed.",
			  vg_name);
		goto bad;
	}
	return _vg_make_handle(cmd, vg, SUCCESS);

bad:
	unlock_and_release_vg(cmd, vg, vg_name);
	/* FIXME: use _vg_make_handle() w/proper error code */
	return NULL;
}

uint64_t extents_from_size(struct cmd_context *cmd, uint64_t size,
			   uint32_t extent_size)
{
	if (size % extent_size) {
		size += extent_size - size % extent_size;
		log_print("Rounding up size to full physical extent %s",
			  display_size(cmd, size));
	}

	if (size > (uint64_t) MAX_EXTENT_COUNT * extent_size) {
		log_error("Volume too large (%s) for extent size %s. "
			  "Upper limit is %s.",
			  display_size(cmd, size),
			  display_size(cmd, (uint64_t) extent_size),
			  display_size(cmd, (uint64_t) MAX_EXTENT_COUNT *
				       extent_size));
		return 0;
	}

	return (uint64_t) size / extent_size;
}

/*
 * Return random integer in [0,max) interval
 *
 * The loop rejects numbers that come from an "incomplete" slice of the
 * RAND_MAX space (considering the number space [0, RAND_MAX] is divided
 * into some "max"-sized slices and at most a single smaller slice,
 * between [n*max, RAND_MAX] for suitable n -- numbers from this last slice
 * are discarded because they could distort the distribution in favour of
 * smaller numbers.
 */
static unsigned _even_rand( unsigned *seed, unsigned max )
{
	unsigned r, ret;

	/* make sure distribution is even */
	do {
		r = (unsigned) rand_r( seed );
		ret = r % max;
	} while ( r - ret > RAND_MAX - max );

	return ret;
}

static dm_bitset_t _bitset_with_random_bits(struct dm_pool *mem, uint32_t num_bits,
					    uint32_t num_set_bits, unsigned *seed)
{
	dm_bitset_t bs;
	unsigned bit_selected;
	char buf[32];
	uint32_t i = num_bits - num_set_bits;

	if (!(bs = dm_bitset_create(mem, (unsigned) num_bits))) {
		log_error("Failed to allocate bitset for setting random bits.");
		return NULL;
	}

        if (!dm_pool_begin_object(mem, 512)) {
                log_error("dm_pool_begin_object failed for random list of bits.");
		dm_pool_free(mem, bs);
                return NULL;
        }

	/* Perform loop num_set_bits times, selecting one bit each time */
	while (i++ < num_bits) {
		/* Select a random bit between 0 and (i-1) inclusive. */
		bit_selected = _even_rand(seed, i);

		/*
		 * If the bit was already set, set the new bit that became
		 * choosable for the first time during this pass.
		 * This maintains a uniform probability distribution by compensating
		 * for being unable to select it until this pass.
		 */
		if (dm_bit(bs, bit_selected))
			bit_selected = i - 1;

		dm_bit_set(bs, bit_selected);

		if (dm_snprintf(buf, sizeof(buf), "%u ", bit_selected) < 0) {
			log_error("snprintf random bit failed.");
			dm_pool_free(mem, bs);
                	return NULL;
		}
		if (!dm_pool_grow_object(mem, buf, strlen(buf))) {
			log_error("Failed to generate list of random bits.");
			dm_pool_free(mem, bs);
                	return NULL;
		}
	}

	if (!dm_pool_grow_object(mem, "\0", 1)) {
		log_error("Failed to finish list of random bits.");
		dm_pool_free(mem, bs);
		return NULL;
	}

	log_debug("Selected %" PRIu32 " random bits from %" PRIu32 ": %s", num_set_bits, num_bits, (char *) dm_pool_end_object(mem));

	return bs;
}

static int _vg_ignore_mdas(struct volume_group *vg, uint32_t num_to_ignore)
{
	struct metadata_area *mda;
	uint32_t mda_used_count = vg_mda_used_count(vg);
	dm_bitset_t mda_to_ignore_bs;
	int r = 1;

	log_debug("Adjusting ignored mdas for %s: %" PRIu32 " of %" PRIu32 " mdas in use "
		  "but %" PRIu32 " required.  Changing %" PRIu32 " mda.",
		  vg->name, mda_used_count, vg_mda_count(vg), vg_mda_copies(vg), num_to_ignore);

	if (!num_to_ignore)
		return 1;

	if (!(mda_to_ignore_bs = _bitset_with_random_bits(vg->vgmem, mda_used_count,
							  num_to_ignore, &vg->cmd->rand_seed)))
		return_0;

	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use)
		if (!mda_is_ignored(mda) && (--mda_used_count,
		    dm_bit(mda_to_ignore_bs, mda_used_count))) {
			mda_set_ignored(mda, 1);
			if (!--num_to_ignore)
				goto out;
		}

	log_error(INTERNAL_ERROR "Unable to find %"PRIu32" metadata areas to ignore "
		  "on volume group %s", num_to_ignore, vg->name);

	r = 0;

out:
	dm_pool_free(vg->vgmem, mda_to_ignore_bs);
	return r;
}

static int _vg_unignore_mdas(struct volume_group *vg, uint32_t num_to_unignore)
{
	struct metadata_area *mda, *tmda;
	uint32_t mda_used_count = vg_mda_used_count(vg);
	uint32_t mda_count = vg_mda_count(vg);
	uint32_t mda_free_count = mda_count - mda_used_count;
	dm_bitset_t mda_to_unignore_bs;
	int r = 1;

	if (!num_to_unignore)
		return 1;

	log_debug("Adjusting ignored mdas for %s: %" PRIu32 " of %" PRIu32 " mdas in use "
		  "but %" PRIu32 " required.  Changing %" PRIu32 " mda.",
		  vg->name, mda_used_count, mda_count, vg_mda_copies(vg), num_to_unignore);

	if (!(mda_to_unignore_bs = _bitset_with_random_bits(vg->vgmem, mda_free_count,
							    num_to_unignore, &vg->cmd->rand_seed)))
		return_0;

	dm_list_iterate_items_safe(mda, tmda, &vg->fid->metadata_areas_ignored)
		if (mda_is_ignored(mda) && (--mda_free_count,
		    dm_bit(mda_to_unignore_bs, mda_free_count))) {
			mda_set_ignored(mda, 0);
			dm_list_move(&vg->fid->metadata_areas_in_use,
				     &mda->list);
			if (!--num_to_unignore)
				goto out;
		}

	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use)
		if (mda_is_ignored(mda) && (--mda_free_count,
		    dm_bit(mda_to_unignore_bs, mda_free_count))) {
			mda_set_ignored(mda, 0);
			if (!--num_to_unignore)
				goto out;
		}

	log_error(INTERNAL_ERROR "Unable to find %"PRIu32" metadata areas to unignore "
		 "on volume group %s", num_to_unignore, vg->name);

	r = 0;

out:
	dm_pool_free(vg->vgmem, mda_to_unignore_bs);
	return r;
}

static int _vg_adjust_ignored_mdas(struct volume_group *vg)
{
	uint32_t mda_copies_used = vg_mda_used_count(vg);

	if (vg->mda_copies == VGMETADATACOPIES_UNMANAGED) {
		/* Ensure at least one mda is in use. */
		if (!mda_copies_used && vg_mda_count(vg) && !_vg_unignore_mdas(vg, 1))
			return_0;
		else
			return 1;
	}


	/* Not an error to have vg_mda_count larger than total mdas. */
	if (vg->mda_copies == VGMETADATACOPIES_ALL ||
	    vg->mda_copies >= vg_mda_count(vg)) {
		/* Use all */
		if (!_vg_unignore_mdas(vg, vg_mda_count(vg) - mda_copies_used))
			return_0;
	} else if (mda_copies_used < vg->mda_copies) {
		if (!_vg_unignore_mdas(vg, vg->mda_copies - mda_copies_used))
			return_0;
	} else if (mda_copies_used > vg->mda_copies)
		if (!_vg_ignore_mdas(vg, mda_copies_used - vg->mda_copies))
			return_0;

	/*
	 * The VGMETADATACOPIES_ALL value will never be written disk.
	 * It is a special cmdline value that means 2 things:
	 * 1. clear all ignore bits in all mdas in this vg
	 * 2. set the "unmanaged" policy going forward for metadata balancing
	 */
	if (vg->mda_copies == VGMETADATACOPIES_ALL)
		vg->mda_copies = VGMETADATACOPIES_UNMANAGED;

	return 1;
}

uint64_t find_min_mda_size(struct dm_list *mdas)
{
	uint64_t min_mda_size = UINT64_MAX, mda_size;
	struct metadata_area *mda;

	dm_list_iterate_items(mda, mdas) {
		if (!mda->ops->mda_total_sectors)
			continue;
		mda_size = mda->ops->mda_total_sectors(mda);
		if (mda_size < min_mda_size)
			min_mda_size = mda_size;
	}

	if (min_mda_size == UINT64_MAX)
		min_mda_size = UINT64_C(0);

	return min_mda_size;
}

static int _move_mdas(struct volume_group *vg_from, struct volume_group *vg_to,
		      struct dm_list *mdas_from, struct dm_list *mdas_to)
{
	struct metadata_area *mda, *mda2;
	int common_mda = 0;

	dm_list_iterate_items_safe(mda, mda2, mdas_from) {
		if (!mda->ops->mda_in_vg) {
			common_mda = 1;
			continue;
		}

		if (!mda->ops->mda_in_vg(vg_from->fid, vg_from, mda)) {
			if (is_orphan_vg(vg_to->name))
				dm_list_del(&mda->list);
			else
				dm_list_move(mdas_to, &mda->list);
		}
	}
	return common_mda;
}

/*
 * Separate metadata areas after splitting a VG.
 * Also accepts orphan VG as destination (for vgreduce).
 */
int vg_split_mdas(struct cmd_context *cmd __attribute__((unused)),
		  struct volume_group *vg_from, struct volume_group *vg_to)
{
	struct dm_list *mdas_from_in_use, *mdas_to_in_use;
	struct dm_list *mdas_from_ignored, *mdas_to_ignored;
	int common_mda = 0;

	mdas_from_in_use = &vg_from->fid->metadata_areas_in_use;
	mdas_from_ignored = &vg_from->fid->metadata_areas_ignored;
	mdas_to_in_use = &vg_to->fid->metadata_areas_in_use;
	mdas_to_ignored = &vg_to->fid->metadata_areas_ignored;

	common_mda = _move_mdas(vg_from, vg_to,
				mdas_from_in_use, mdas_to_in_use);
	common_mda = _move_mdas(vg_from, vg_to,
				mdas_from_ignored, mdas_to_ignored);

	if ((dm_list_empty(mdas_from_in_use) &&
	     dm_list_empty(mdas_from_ignored)) ||
	    ((!is_orphan_vg(vg_to->name) &&
	      dm_list_empty(mdas_to_in_use) &&
	      dm_list_empty(mdas_to_ignored))))
		return common_mda;

	return 1;
}

static int _wipe_sb(struct device *dev, const char *type, const char *name,
		    int wipe_len, struct pvcreate_params *pp,
		    int (*func)(struct device *dev, uint64_t *signature))
{
	int wipe;
	uint64_t superblock;

	wipe = func(dev, &superblock);
	if (wipe == -1) {
		log_error("Fatal error while trying to detect %s on %s.",
			  type, name);
		return 0;
	}

	if (wipe == 0)
		return 1;

	/* Specifying --yes => do not ask. */
	if (!pp->yes && (pp->force == PROMPT) &&
	    yes_no_prompt("WARNING: %s detected on %s. Wipe it? [y/n] ",
			  type, name) != 'y') {
		log_error("Aborting pvcreate on %s.", name);
		return 0;
	}

	log_print("Wiping %s on %s.", type, name);
	if (!dev_set(dev, superblock, wipe_len, 0)) {
		log_error("Failed to wipe %s on %s.", type, name);
		return 0;
	}

	return 1;
}

/*
 * See if we may pvcreate on this device.
 * 0 indicates we may not.
 */
static int pvcreate_check(struct cmd_context *cmd, const char *name,
			  struct pvcreate_params *pp)
{
	struct physical_volume *pv;
	struct device *dev;

	/* FIXME Check partition type is LVM unless --force is given */

	/* Is there a pv here already? */
	pv = pv_read(cmd, name, 0, 0);

	/*
	 * If a PV has no MDAs it may appear to be an orphan until the
	 * metadata is read off another PV in the same VG.  Detecting
	 * this means checking every VG by scanning every PV on the
	 * system.
	 */
	if (pv && is_orphan(pv) && !dm_list_size(&pv->fid->metadata_areas_in_use)) {
		free_pv_fid(pv);
		if (!scan_vgs_for_pvs(cmd, 0))
			return_0;
		pv = pv_read(cmd, name, 0, 0);
	}

	/* Allow partial & exported VGs to be destroyed. */
	/* We must have -ff to overwrite a non orphan */
	if (pv && !is_orphan(pv) && pp->force != DONT_PROMPT_OVERRIDE) {
		log_error("Can't initialize physical volume \"%s\" of "
			  "volume group \"%s\" without -ff", name, pv_vg_name(pv));
		goto bad;
	}

	/* prompt */
	if (pv && !is_orphan(pv) && !pp->yes &&
	    yes_no_prompt(_really_init, name, pv_vg_name(pv)) == 'n') {
		log_error("%s: physical volume not initialized", name);
		goto bad;
	}

	if (sigint_caught())
		goto_bad;

	dev = dev_cache_get(name, cmd->filter);

	/* Is there an md superblock here? */
	/* FIXME: still possible issues here - rescan cache? */
	if (!dev && md_filtering()) {
		if (!refresh_filters(cmd))
			goto_bad;

		init_md_filtering(0);
		dev = dev_cache_get(name, cmd->filter);
		init_md_filtering(1);
	}

	if (!dev) {
		log_error("Device %s not found (or ignored by filtering).", name);
		goto bad;
	}

	/*
	 * This test will fail if the device belongs to an MD array.
	 */
	if (!dev_test_excl(dev)) {
		/* FIXME Detect whether device-mapper itself is still using it */
		log_error("Can't open %s exclusively.  Mounted filesystem?",
			  name);
		goto bad;
	}

	if (!_wipe_sb(dev, "software RAID md superblock", name, 4, pp, dev_is_md))
		goto_bad;

	if (!_wipe_sb(dev, "swap signature", name, 10, pp, dev_is_swap))
		goto_bad;

	if (!_wipe_sb(dev, "LUKS signature", name, 8, pp, dev_is_luks))
		goto_bad;

	if (sigint_caught())
		goto_bad;

	if (pv && !is_orphan(pv) && pp->force) {
		log_warn("WARNING: Forcing physical volume creation on "
			  "%s%s%s%s", name,
			  !is_orphan(pv) ? " of volume group \"" : "",
			  !is_orphan(pv) ? pv_vg_name(pv) : "",
			  !is_orphan(pv) ? "\"" : "");
	}

	free_pv_fid(pv);
	return 1;

bad:
	free_pv_fid(pv);
	return 0;
}

void pvcreate_params_set_defaults(struct pvcreate_params *pp)
{
	memset(pp, 0, sizeof(*pp));
	pp->zero = 1;
	pp->size = 0;
	pp->data_alignment = UINT64_C(0);
	pp->data_alignment_offset = UINT64_C(0);
	pp->pvmetadatacopies = DEFAULT_PVMETADATACOPIES;
	pp->pvmetadatasize = DEFAULT_PVMETADATASIZE;
	pp->labelsector = DEFAULT_LABELSECTOR;
	pp->idp = 0;
	pp->pe_start = 0;
	pp->extent_count = 0;
	pp->extent_size = 0;
	pp->restorefile = 0;
	pp->force = PROMPT;
	pp->yes = 0;
	pp->metadataignore = DEFAULT_PVMETADATAIGNORE;
}


static int _pvcreate_write(struct cmd_context *cmd, struct pv_to_create *pvc)
{
	int zero = pvc->pp->zero;
	struct physical_volume *pv = pvc->pv;
	struct device *dev = pv->dev;
	const char *pv_name = dev_name(dev);

	/* Wipe existing label first */
	if (!label_remove(pv_dev(pv))) {
		log_error("Failed to wipe existing label on %s", pv_name);
		return 0;
	}

	if (zero) {
		log_verbose("Zeroing start of device %s", pv_name);
		if (!dev_open_quiet(dev)) {
			log_error("%s not opened: device not zeroed", pv_name);
			return 0;
		}

		if (!dev_set(dev, UINT64_C(0), (size_t) 2048, 0)) {
			log_error("%s not wiped: aborting", pv_name);
			dev_close(dev);
			return 0;
		}
		dev_close(dev);
	}

	log_error("Writing physical volume data to disk \"%s\"",
			 pv_name);

	if (!(pv_write(cmd, pv, 1))) {
		log_error("Failed to write physical volume \"%s\"", pv_name);
		return 0;
	}

	log_print("Physical volume \"%s\" successfully created", pv_name);
	return 1;
}

/*
 * pvcreate_single() - initialize a device with PV label and metadata area
 *
 * Parameters:
 * - pv_name: device path to initialize
 * - pp: parameters to pass to pv_create; if NULL, use default values
 *
 * Returns:
 * NULL: error
 * struct physical_volume * (non-NULL): handle to physical volume created
 */
struct physical_volume * pvcreate_single(struct cmd_context *cmd,
					 const char *pv_name,
					 struct pvcreate_params *pp,
					 int write_now)
{
	struct physical_volume *pv = NULL;
	struct device *dev;
	struct dm_list mdas;
	struct pvcreate_params default_pp;
	char buffer[64] __attribute__((aligned(8)));

	pvcreate_params_set_defaults(&default_pp);
	if (!pp)
		pp = &default_pp;

	if (pp->idp) {
		if ((dev = lvmcache_device_from_pvid(cmd, pp->idp, NULL, NULL)) &&
		    (dev != dev_cache_get(pv_name, cmd->filter))) {
			if (!id_write_format((const struct id*)&pp->idp->uuid,
			    buffer, sizeof(buffer)))
				goto_bad;
			log_error("uuid %s already in use on \"%s\"", buffer,
				  dev_name(dev));
			goto bad;;
		}
	}

	if (!pvcreate_check(cmd, pv_name, pp))
		goto_bad;

	if (sigint_caught())
		goto_bad;

	if (!(dev = dev_cache_get(pv_name, cmd->filter))) {
		log_error("%s: Couldn't find device.  Check your filters?",
			  pv_name);
		goto bad;
	}

	dm_list_init(&mdas);

	if (!(pv = pv_create(cmd, dev, pp->idp, pp->size,
			     pp->data_alignment, pp->data_alignment_offset,
			     pp->pe_start ? pp->pe_start : PV_PE_START_CALC,
			     pp->extent_count, pp->extent_size,
			     pp->labelsector, pp->pvmetadatacopies,
			     pp->pvmetadatasize, pp->metadataignore))) {
		log_error("Failed to setup physical volume \"%s\"", pv_name);
		goto bad;
	}

	log_verbose("Set up physical volume for \"%s\" with %" PRIu64
		    " available sectors", pv_name, pv_size(pv));

	if (write_now) {
		struct pv_to_create pvc;
		pvc.pp = pp;
		pvc.pv = pv;
		if (!_pvcreate_write(cmd, &pvc))
			goto bad;
	} else {
		pv->status |= UNLABELLED_PV;
	}

	return pv;

bad:
	free_pv_fid(pv);
	return NULL;
}

static struct physical_volume *_alloc_pv(struct dm_pool *mem, struct device *dev)
{
	struct physical_volume *pv = dm_pool_zalloc(mem, sizeof(*pv));

	if (!pv)
		return_NULL;

	pv_set_fid(pv, NULL);
	pv->pe_size = 0;
	pv->pe_start = 0;
	pv->pe_count = 0;
	pv->pe_alloc_count = 0;
	pv->pe_align = 0;
	pv->pe_align_offset = 0;
	pv->fmt = NULL;
	pv->dev = dev;

	pv->status = ALLOCATABLE_PV;

	dm_list_init(&pv->tags);
	dm_list_init(&pv->segments);

	return pv;
}

/**
 * pv_create - initialize a physical volume for use with a volume group
 *
 * @fmt: format type
 * @dev: PV device to initialize
 * @size: size of the PV in sectors
 * @data_alignment: requested alignment of data
 * @data_alignment_offset: requested offset to aligned data
 * @pe_start: physical extent start
 * @existing_extent_count
 * @existing_extent_size
 * @pvmetadatacopies
 * @pvmetadatasize
 * @mdas
 *
 * Returns:
 *   PV handle - physical volume initialized successfully
 *   NULL - invalid parameter or problem initializing the physical volume
 *
 * Note:
 *   FIXME: shorten argument list and replace with explict 'set' functions
 */
struct physical_volume *pv_create(const struct cmd_context *cmd,
				  struct device *dev,
				  struct id *id, uint64_t size,
				  unsigned long data_alignment,
				  unsigned long data_alignment_offset,
				  uint64_t pe_start,
				  uint32_t existing_extent_count,
				  uint32_t existing_extent_size,
				  uint64_t label_sector,
				  unsigned pvmetadatacopies,
				  uint64_t pvmetadatasize,
				  unsigned metadataignore)
{
	const struct format_type *fmt = cmd->fmt;
	struct dm_pool *mem = fmt->cmd->mem;
	struct physical_volume *pv = _alloc_pv(mem, dev);
	unsigned mda_index;

	if (!pv)
		return NULL;

	if (id)
		memcpy(&pv->id, id, sizeof(*id));
	else if (!id_create(&pv->id)) {
		log_error("Failed to create random uuid for %s.",
			  dev_name(dev));
		goto bad;
	}

	if (!dev_get_size(pv->dev, &pv->size)) {
		log_error("%s: Couldn't get size.", pv_dev_name(pv));
		goto bad;
	}

	if (size) {
		if (size > pv->size)
			log_warn("WARNING: %s: Overriding real size. "
				  "You could lose data.", pv_dev_name(pv));
		log_verbose("%s: Pretending size is %" PRIu64 " sectors.",
			    pv_dev_name(pv), size);
		pv->size = size;
	}

	if (pv->size < pv_min_size()) {
		log_error("%s: Size must exceed minimum of %" PRIu64 " sectors.",
			  pv_dev_name(pv), pv_min_size());
		goto bad;
	}

	if (pv->size < data_alignment) {
		log_error("%s: Data alignment must not exceed device size.",
			  pv_dev_name(pv));
		goto bad;
	}

	struct pv_list *pvl;
	if (!(pvl = dm_pool_zalloc(mem, sizeof(*pvl)))) {
		log_error("pv_list allocation in pv_create failed");
		goto bad;
	}

	pvl->pv = pv;
	add_pvl_to_vgs(fmt->orphan_vg, pvl);
	fmt->orphan_vg->extent_count += pv->pe_count;
	fmt->orphan_vg->free_count += pv->pe_count;

	pv->fmt = fmt;
	pv->vg_name = fmt->orphan_vg_name;

	if (!fmt->ops->pv_initialise(fmt, label_sector, pe_start,
				     existing_extent_count, existing_extent_size,
				     data_alignment, data_alignment_offset, pv)) {
		log_error("Format-specific initialisation of physical "
			  "volume %s failed.", pv_dev_name(pv));
		goto bad;
	}

	for (mda_index = 0; mda_index < pvmetadatacopies; mda_index++) {
		if (pv->fmt->ops->pv_add_metadata_area &&
		    !pv->fmt->ops->pv_add_metadata_area(pv->fmt, pv,
					pe_start != PV_PE_START_CALC,
					mda_index, pvmetadatasize,
					metadataignore)) {
			log_error("Failed to add metadata area for "
				  "new physical volume %s", pv_dev_name(pv));
			goto bad;
		}
	}

	return pv;

      bad:
	free_pv_fid(pv);
	dm_pool_free(mem, pv);
	return NULL;
}

/* FIXME: liblvm todo - make into function that returns handle */
struct pv_list *find_pv_in_vg(const struct volume_group *vg,
			      const char *pv_name)
{
	return _find_pv_in_vg(vg, pv_name);
}

static struct pv_list *_find_pv_in_vg(const struct volume_group *vg,
				      const char *pv_name)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, &vg->pvs)
		if (pvl->pv->dev == dev_cache_get(pv_name, vg->cmd->filter))
			return pvl;

	return NULL;
}

struct pv_list *find_pv_in_pv_list(const struct dm_list *pl,
				   const struct physical_volume *pv)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, pl)
		if (pvl->pv == pv)
			return pvl;

	return NULL;
}

int pv_is_in_vg(struct volume_group *vg, struct physical_volume *pv)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, &vg->pvs)
		if (pv == pvl->pv)
			 return 1;

	return 0;
}

static struct pv_list *_find_pv_in_vg_by_uuid(const struct volume_group *vg,
					      const struct id *id)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, &vg->pvs)
		if (id_equal(&pvl->pv->id, id))
			return pvl;

	return NULL;
}

/**
 * find_pv_in_vg_by_uuid - Find PV in VG by PV UUID
 * @vg: volume group to search
 * @id: UUID of the PV to match
 *
 * Returns:
 *   struct pv_list within owning struct volume_group - if UUID of PV found in VG
 *   NULL - invalid parameter or UUID of PV not found in VG
 *
 * Note
 *   FIXME - liblvm todo - make into function that takes VG handle
 */
struct pv_list *find_pv_in_vg_by_uuid(const struct volume_group *vg,
				      const struct id *id)
{
	return _find_pv_in_vg_by_uuid(vg, id);
}

struct lv_list *find_lv_in_vg(const struct volume_group *vg,
			      const char *lv_name)
{
	struct lv_list *lvl;
	const char *ptr;

	/* Use last component */
	if ((ptr = strrchr(lv_name, '/')))
		ptr++;
	else
		ptr = lv_name;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (!strcmp(lvl->lv->name, ptr))
			return lvl;

	return NULL;
}

struct lv_list *find_lv_in_lv_list(const struct dm_list *ll,
				   const struct logical_volume *lv)
{
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, ll)
		if (lvl->lv == lv)
			return lvl;

	return NULL;
}

struct lv_list *find_lv_in_vg_by_lvid(struct volume_group *vg,
				      const union lvid *lvid)
{
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (!strncmp(lvl->lv->lvid.s, lvid->s, sizeof(*lvid)))
			return lvl;

	return NULL;
}

struct logical_volume *find_lv(const struct volume_group *vg,
			       const char *lv_name)
{
	struct lv_list *lvl = find_lv_in_vg(vg, lv_name);
	return lvl ? lvl->lv : NULL;
}

struct physical_volume *find_pv(struct volume_group *vg, struct device *dev)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, &vg->pvs)
		if (dev == pvl->pv->dev)
			return pvl->pv;

	return NULL;
}

/* FIXME: liblvm todo - make into function that returns handle */
struct physical_volume *find_pv_by_name(struct cmd_context *cmd,
					const char *pv_name)
{
	return _find_pv_by_name(cmd, pv_name);
}


static struct physical_volume *_find_pv_by_name(struct cmd_context *cmd,
			 			const char *pv_name)
{
	struct physical_volume *pv;

	if (!(pv = _pv_read(cmd, cmd->mem, pv_name, NULL, 1, 0))) {
		log_error("Physical volume %s not found", pv_name);
		goto bad;
	}

	if (is_orphan_vg(pv->vg_name) && !dm_list_size(&pv->fid->metadata_areas_in_use)) {
		/* If a PV has no MDAs - need to search all VGs for it */
		if (!scan_vgs_for_pvs(cmd, 1))
			goto_bad;
		free_pv_fid(pv);
		if (!(pv = _pv_read(cmd, cmd->mem, pv_name, NULL, 1, 0))) {
			log_error("Physical volume %s not found", pv_name);
			goto bad;
		}
	}

	if (is_orphan_vg(pv->vg_name)) {
		log_error("Physical volume %s not in a volume group", pv_name);
		goto bad;
	}

	return pv;

bad:
	free_pv_fid(pv);
	return NULL;
}

/* Find segment at a given logical extent in an LV */
struct lv_segment *find_seg_by_le(const struct logical_volume *lv, uint32_t le)
{
	struct lv_segment *seg;

	dm_list_iterate_items(seg, &lv->segments)
		if (le >= seg->le && le < seg->le + seg->len)
			return seg;

	return NULL;
}

struct lv_segment *first_seg(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	dm_list_iterate_items(seg, &lv->segments)
		return seg;

	return NULL;
}

struct lv_segment *last_seg(const struct logical_volume *lv)
{
       struct lv_segment *seg;

       dm_list_iterate_back_items(seg, &lv->segments)
               return seg;

       return NULL;
}

int vg_remove_mdas(struct volume_group *vg)
{
	struct metadata_area *mda;

	/* FIXME Improve recovery situation? */
	/* Remove each copy of the metadata */
	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use) {
		if (mda->ops->vg_remove &&
		    !mda->ops->vg_remove(vg->fid, vg, mda))
			return_0;
	}

	return 1;
}

/*
 * Determine whether two vgs are compatible for merging.
 */
int vgs_are_compatible(struct cmd_context *cmd __attribute__((unused)),
		       struct volume_group *vg_from,
		       struct volume_group *vg_to)
{
	struct lv_list *lvl1, *lvl2;
	struct pv_list *pvl;
	const char *name1, *name2;

	if (lvs_in_vg_activated(vg_from)) {
		log_error("Logical volumes in \"%s\" must be inactive",
			  vg_from->name);
		return 0;
	}

	/* Check compatibility */
	if (vg_to->extent_size != vg_from->extent_size) {
		log_error("Extent sizes differ: %d (%s) and %d (%s)",
			  vg_to->extent_size, vg_to->name,
			  vg_from->extent_size, vg_from->name);
		return 0;
	}

	if (vg_to->max_pv &&
	    (vg_to->max_pv < vg_to->pv_count + vg_from->pv_count)) {
		log_error("Maximum number of physical volumes (%d) exceeded "
			  " for \"%s\" and \"%s\"", vg_to->max_pv, vg_to->name,
			  vg_from->name);
		return 0;
	}

	if (vg_to->max_lv &&
	    (vg_to->max_lv < vg_visible_lvs(vg_to) + vg_visible_lvs(vg_from))) {
		log_error("Maximum number of logical volumes (%d) exceeded "
			  " for \"%s\" and \"%s\"", vg_to->max_lv, vg_to->name,
			  vg_from->name);
		return 0;
	}

	/* Metadata types must be the same */
	if (vg_to->fid->fmt != vg_from->fid->fmt) {
		log_error("Metadata types differ for \"%s\" and \"%s\"",
			  vg_to->name, vg_from->name);
		return 0;
	}

	/* Clustering attribute must be the same */
	if (vg_is_clustered(vg_to) != vg_is_clustered(vg_from)) {
		log_error("Clustered attribute differs for \"%s\" and \"%s\"",
			  vg_to->name, vg_from->name);
		return 0;
	}

	/* Check no conflicts with LV names */
	dm_list_iterate_items(lvl1, &vg_to->lvs) {
		name1 = lvl1->lv->name;

		dm_list_iterate_items(lvl2, &vg_from->lvs) {
			name2 = lvl2->lv->name;

			if (!strcmp(name1, name2)) {
				log_error("Duplicate logical volume "
					  "name \"%s\" "
					  "in \"%s\" and \"%s\"",
					  name1, vg_to->name, vg_from->name);
				return 0;
			}
		}
	}

	/* Check no PVs are constructed from either VG */
	dm_list_iterate_items(pvl, &vg_to->pvs) {
		if (pv_uses_vg(pvl->pv, vg_from)) {
			log_error("Physical volume %s might be constructed "
				  "from same volume group %s.",
				  pv_dev_name(pvl->pv), vg_from->name);
			return 0;
		}
	}

	dm_list_iterate_items(pvl, &vg_from->pvs) {
		if (pv_uses_vg(pvl->pv, vg_to)) {
			log_error("Physical volume %s might be constructed "
				  "from same volume group %s.",
				  pv_dev_name(pvl->pv), vg_to->name);
			return 0;
		}
	}

	return 1;
}

struct _lv_postorder_baton {
	int (*fn)(struct logical_volume *lv, void *data);
	void *data;
};

static int _lv_postorder_visit(struct logical_volume *,
			       int (*fn)(struct logical_volume *lv, void *data),
			       void *data);

static int _lv_each_dependency(struct logical_volume *lv,
			       int (*fn)(struct logical_volume *lv, void *data),
			       void *data)
{
	unsigned i, s;
	struct lv_segment *lvseg;

	struct logical_volume *deps[] = {
		(lv->rdevice && lv != lv->rdevice->lv) ? lv->rdevice->lv : 0,
		(lv->rdevice && lv != lv->rdevice->slog) ? lv->rdevice->slog : 0,
		lv->snapshot ? lv->snapshot->origin : 0,
		lv->snapshot ? lv->snapshot->cow : 0 };
	for (i = 0; i < sizeof(deps) / sizeof(*deps); ++i) {
		if (deps[i] && !fn(deps[i], data))
			return_0;
	}

	dm_list_iterate_items(lvseg, &lv->segments) {
		if (lvseg->log_lv && !fn(lvseg->log_lv, data))
			return_0;
		if (lvseg->rlog_lv && !fn(lvseg->rlog_lv, data))
			return_0;
		if (lvseg->pool_lv && !fn(lvseg->pool_lv, data))
			return_0;
		if (lvseg->metadata_lv && !fn(lvseg->metadata_lv, data))
			return_0;
		for (s = 0; s < lvseg->area_count; ++s) {
			if (seg_type(lvseg, s) == AREA_LV && !fn(seg_lv(lvseg,s), data))
				return_0;
		}
	}
	return 1;
}

static int _lv_postorder_cleanup(struct logical_volume *lv, void *data)
{
	if (!(lv->status & POSTORDER_FLAG))
		return 1;
	lv->status &= ~POSTORDER_FLAG;

	if (!_lv_each_dependency(lv, _lv_postorder_cleanup, data))
		return_0;
	return 1;
}

static int _lv_postorder_level(struct logical_volume *lv, void *data)
{
	struct _lv_postorder_baton *baton = data;
	return _lv_postorder_visit(lv, baton->fn, baton->data);
};

static int _lv_postorder_visit(struct logical_volume *lv,
			       int (*fn)(struct logical_volume *lv, void *data),
			       void *data)
{
	struct _lv_postorder_baton baton;
	int r;

	if (lv->status & POSTORDER_FLAG)
		return 1;
	if (lv->status & POSTORDER_OPEN_FLAG)
		return 1; // a data structure loop has closed...
	lv->status |= POSTORDER_OPEN_FLAG;

	baton.fn = fn;
	baton.data = data;
	r = _lv_each_dependency(lv, _lv_postorder_level, &baton);

	if (r)
		r = fn(lv, data);

	lv->status &= ~POSTORDER_OPEN_FLAG;
	lv->status |= POSTORDER_FLAG;

	return r;
}

/*
 * This will walk the LV dependency graph in depth-first order and in the
 * postorder, call a callback function "fn". The void *data is passed along all
 * the calls. The callback may return zero to indicate an error and terminate
 * the depth-first walk. The error is propagated to return value of
 * _lv_postorder.
 */
static int _lv_postorder(struct logical_volume *lv,
			       int (*fn)(struct logical_volume *lv, void *data),
			       void *data)
{
	int r;
	int pool_locked = dm_pool_locked(lv->vg->vgmem);

	if (pool_locked && !dm_pool_unlock(lv->vg->vgmem, 0))
		return_0;

	r = _lv_postorder_visit(lv, fn, data);
	_lv_postorder_cleanup(lv, 0);

	if (pool_locked && !dm_pool_lock(lv->vg->vgmem, 0))
		return_0;

	return r;
}

/*
 * Calls _lv_postorder() on each LV from VG. Avoids duplicate transitivity visits.
 * Clears with _lv_postorder_cleanup() when all LVs were visited by postorder.
 */
static int _lv_postorder_vg(struct volume_group *vg,
			    int (*fn)(struct logical_volume *lv, void *data),
			    void *data)
{
	struct lv_list *lvl;
	int r = 1;
	int pool_locked = dm_pool_locked(vg->vgmem);

	if (pool_locked && !dm_pool_unlock(vg->vgmem, 0))
		return_0;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (!_lv_postorder_visit(lvl->lv, fn, data)) {
			stack;
			r = 0;
		}

	dm_list_iterate_items(lvl, &vg->lvs)
		_lv_postorder_cleanup(lvl->lv, 0);

	if (pool_locked && !dm_pool_lock(vg->vgmem, 0))
		return_0;

	return r;
}

struct _lv_mark_if_partial_baton {
	int partial;
};

static int _lv_mark_if_partial_collect(struct logical_volume *lv, void *data)
{
	struct _lv_mark_if_partial_baton *baton = data;
	if (lv->status & PARTIAL_LV)
		baton->partial = 1;

	return 1;
}

static int _lv_mark_if_partial_single(struct logical_volume *lv, void *data)
{
	unsigned s;
	struct _lv_mark_if_partial_baton baton;
	struct lv_segment *lvseg;

	dm_list_iterate_items(lvseg, &lv->segments) {
		for (s = 0; s < lvseg->area_count; ++s) {
			if (seg_type(lvseg, s) == AREA_PV) {
				if (is_missing_pv(seg_pv(lvseg, s)))
					lv->status |= PARTIAL_LV;
			}
		}
	}

	baton.partial = 0;
	_lv_each_dependency(lv, _lv_mark_if_partial_collect, &baton);

	if (baton.partial)
		lv->status |= PARTIAL_LV;

	return 1;
}

/*
 * Mark LVs with missing PVs using PARTIAL_LV status flag. The flag is
 * propagated transitively, so LVs referencing other LVs are marked
 * partial as well, if any of their referenced LVs are marked partial.
 */
int vg_mark_partial_lvs(struct volume_group *vg, int clear)
{
	struct lv_list *lvl;

	if (clear)
		dm_list_iterate_items(lvl, &vg->lvs)
			lvl->lv->status &= ~PARTIAL_LV;

	if (!_lv_postorder_vg(vg, _lv_mark_if_partial_single, NULL))
		return_0;
	return 1;
}

/*
 * Be sure that all PV devices have cached read ahead in dev-cache
 * Currently it takes read_ahead from first PV segment only
 */
static int _lv_read_ahead_single(struct logical_volume *lv, void *data)
{
	struct lv_segment *seg = first_seg(lv);
	uint32_t seg_read_ahead = 0, *read_ahead = data;

	if (seg && seg->area_count && seg_type(seg, 0) == AREA_PV)
		dev_get_read_ahead(seg_pv(seg, 0)->dev, &seg_read_ahead);

	if (seg_read_ahead > *read_ahead)
		*read_ahead = seg_read_ahead;

	return 1;
}

/*
 * Calculate readahead for logical volume from underlying PV devices.
 * If read_ahead is NULL, only ensure that readahead of PVs are preloaded
 * into PV struct device in dev cache.
 */
void lv_calculate_readahead(const struct logical_volume *lv, uint32_t *read_ahead)
{
	uint32_t _read_ahead = 0;

	if (lv->read_ahead == DM_READ_AHEAD_AUTO)
		_lv_postorder((struct logical_volume *)lv, _lv_read_ahead_single, &_read_ahead);

	if (read_ahead) {
		log_debug("Calculated readahead of LV %s is %u", lv->name, _read_ahead);
		*read_ahead = _read_ahead;
	}
}

struct validate_hash {
	struct dm_hash_table *lvname;
	struct dm_hash_table *lvid;
	struct dm_hash_table *pvid;
};

/*
 * Check that an LV and all its PV references are correctly listed in vg->lvs
 * and vg->pvs, respectively. This only looks at a single LV, but *not* at the
 * LVs it is using. To do the latter, you should use _lv_postorder with this
 * function. C.f. vg_validate.
 */
static int _lv_validate_references_single(struct logical_volume *lv, void *data)
{
	struct volume_group *vg = lv->vg;
	struct validate_hash *vhash = data;
	struct lv_segment *lvseg;
	struct physical_volume *pv;
	unsigned s;
	int r = 1;

	if (lv != dm_hash_lookup_binary(vhash->lvid, &lv->lvid.id[1],
					sizeof(lv->lvid.id[1]))) {
		log_error(INTERNAL_ERROR
			  "Referenced LV %s not listed in VG %s.",
			  lv->name, vg->name);
		r = 0;
	}

	dm_list_iterate_items(lvseg, &lv->segments) {
		for (s = 0; s < lvseg->area_count; ++s) {
			if (seg_type(lvseg, s) != AREA_PV)
				continue;
			pv = seg_pv(lvseg, s);
			/* look up the reference in vg->pvs */
			if (pv != dm_hash_lookup_binary(vhash->pvid, &pv->id,
							sizeof(pv->id))) {
				log_error(INTERNAL_ERROR
					  "Referenced PV %s not listed in VG %s.",
					  pv_dev_name(pv), vg->name);
				r = 0;
			}
		}
	}

	return r;
}

int vg_validate(struct volume_group *vg)
{
	struct pv_list *pvl;
	struct lv_list *lvl;
	struct lv_segment *seg;
	char uuid[64] __attribute__((aligned(8)));
	int r = 1;
	uint32_t hidden_lv_count = 0, lv_count = 0, lv_visible_count = 0;
	uint32_t pv_count = 0;
	uint32_t num_snapshots = 0;
	struct validate_hash vhash = { NULL };

	if (vg->alloc == ALLOC_CLING_BY_TAGS) {
		log_error(INTERNAL_ERROR "VG %s allocation policy set to invalid cling_by_tags.",
			  vg->name);
		r = 0;
	}

	/* FIXME Also check there's no data/metadata overlap */
	if (!(vhash.pvid = dm_hash_create(vg->pv_count))) {
		log_error("Failed to allocate pvid hash.");
		return 0;
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (++pv_count > vg->pv_count) {
			log_error(INTERNAL_ERROR "PV list corruption detected in VG %s.", vg->name);
			/* FIXME Dump list structure? */
			r = 0;
		}

		if (pvl->pv->vg != vg) {
			log_error(INTERNAL_ERROR "VG %s PV list entry points "
				  "to different VG %s.", vg->name,
				  pvl->pv->vg ? pvl->pv->vg->name : "NULL");
			r = 0;
		}

		if (strcmp(pvl->pv->vg_name, vg->name)) {
			log_error(INTERNAL_ERROR "VG name for PV %s is corrupted.",
				  pv_dev_name(pvl->pv));
			r = 0;
		}

		if (dm_hash_lookup_binary(vhash.pvid, &pvl->pv->id,
					  sizeof(pvl->pv->id))) {
			if (!id_write_format(&pvl->pv->id, uuid,
					     sizeof(uuid)))
				stack;
			log_error(INTERNAL_ERROR "Duplicate PV id "
				  "%s detected for %s in %s.",
				  uuid, pv_dev_name(pvl->pv),
				  vg->name);
			r = 0;
		}

		if (!dm_hash_insert_binary(vhash.pvid, &pvl->pv->id,
					   sizeof(pvl->pv->id), pvl->pv)) {
			log_error("Failed to hash pvid.");
			r = 0;
			break;
		}
	}


	if (!check_pv_segments(vg)) {
		log_error(INTERNAL_ERROR "PV segments corrupted in %s.",
			  vg->name);
		r = 0;
	}

	/*
	 * Count all non-snapshot invisible LVs
	 */
	dm_list_iterate_items(lvl, &vg->lvs) {
		lv_count++;

		if (lv_is_cow(lvl->lv))
			num_snapshots++;

		if (lv_is_visible(lvl->lv))
			lv_visible_count++;

		if (!check_lv_segments(lvl->lv, 0)) {
			log_error(INTERNAL_ERROR "LV segments corrupted in %s.",
				  lvl->lv->name);
			r = 0;
		}

		if (lvl->lv->alloc == ALLOC_CLING_BY_TAGS) {
			log_error(INTERNAL_ERROR "LV %s allocation policy set to invalid cling_by_tags.",
				  lvl->lv->name);
			r = 0;
		}

		if (lvl->lv->status & VISIBLE_LV)
			continue;

		/* snapshots */
		if (lv_is_cow(lvl->lv))
			continue;

		/* virtual origins are always hidden */
		if (lv_is_origin(lvl->lv) && !lv_is_virtual_origin(lvl->lv))
			continue;

		/* count other non-snapshot invisible volumes */
		hidden_lv_count++;

		/*
		 *  FIXME: add check for unreferenced invisible LVs
		 *   - snapshot cow & origin
		 *   - mirror log & images
		 *   - mirror conversion volumes (_mimagetmp*)
		 */
	}

	/*
	 * all volumes = visible LVs + snapshot_cows + invisible LVs
	 */
	if (lv_count != lv_visible_count + num_snapshots + hidden_lv_count) {
		log_error(INTERNAL_ERROR "#internal LVs (%u) != #LVs (%"
			  PRIu32 ") + #snapshots (%" PRIu32 ") + #internal LVs (%u) in VG %s",
			  lv_count, lv_visible_count,
			  num_snapshots, hidden_lv_count, vg->name);
		r = 0;
	}

	/* Avoid endless loop if lv->segments list is corrupt */
	if (!r)
		goto out;

	if (!(vhash.lvname = dm_hash_create(lv_count))) {
		log_error("Failed to allocate lv_name hash");
		r = 0;
		goto out;
	}

	if (!(vhash.lvid = dm_hash_create(lv_count))) {
		log_error("Failed to allocate uuid hash");
		r = 0;
		goto out;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (dm_hash_lookup(vhash.lvname, lvl->lv->name)) {
			log_error(INTERNAL_ERROR
				  "Duplicate LV name %s detected in %s.",
				  lvl->lv->name, vg->name);
			r = 0;
		}

		if (dm_hash_lookup_binary(vhash.lvid, &lvl->lv->lvid.id[1],
					  sizeof(lvl->lv->lvid.id[1]))) {
			if (!id_write_format(&lvl->lv->lvid.id[1], uuid,
					     sizeof(uuid)))
				stack;
			log_error(INTERNAL_ERROR "Duplicate LV id "
				  "%s detected for %s in %s.",
				  uuid, lvl->lv->name, vg->name);
			r = 0;
		}

		if (!check_lv_segments(lvl->lv, 1)) {
			log_error(INTERNAL_ERROR "LV segments corrupted in %s.",
				  lvl->lv->name);
			r = 0;
		}

		if (!dm_hash_insert(vhash.lvname, lvl->lv->name, lvl)) {
			log_error("Failed to hash lvname.");
			r = 0;
			break;
		}

		if (!dm_hash_insert_binary(vhash.lvid, &lvl->lv->lvid.id[1],
					   sizeof(lvl->lv->lvid.id[1]), lvl->lv)) {
			log_error("Failed to hash lvid.");
			r = 0;
			break;
		}
	}

	if (!_lv_postorder_vg(vg, _lv_validate_references_single, &vhash)) {
		stack;
		r = 0;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (!(lvl->lv->status & PVMOVE))
			continue;
		dm_list_iterate_items(seg, &lvl->lv->segments) {
			if (seg_is_mirrored(seg)) {
				if (seg->area_count != 2) {
					log_error(INTERNAL_ERROR
						  "Segment in %s is not 2-way.",
						  lvl->lv->name);
					r = 0;
				}
			} else if (seg->area_count != 1) {
				log_error(INTERNAL_ERROR
					  "Segment in %s has wrong number of areas: %d.",
					  lvl->lv->name, seg->area_count);
				r = 0;
			}
		}
	}

	if (!(vg->fid->fmt->features & FMT_UNLIMITED_VOLS) &&
	    (!vg->max_lv || !vg->max_pv)) {
		log_error(INTERNAL_ERROR "Volume group %s has limited PV/LV count"
			  " but limit is not set.", vg->name);
		r = 0;
	}

	if (vg_max_lv_reached(vg))
		stack;
out:
	if (vhash.lvid)
		dm_hash_destroy(vhash.lvid);
	if (vhash.lvname)
		dm_hash_destroy(vhash.lvname);
	if (vhash.pvid)
		dm_hash_destroy(vhash.pvid);

	return r;
}

/*
 * After vg_write() returns success,
 * caller MUST call either vg_commit() or vg_revert()
 */
int vg_write(struct volume_group *vg)
{
	struct dm_list *mdah;
        struct pv_to_create *pv_to_create;
	struct metadata_area *mda;

	if (!vg_validate(vg))
		return_0;

	if (vg->status & PARTIAL_VG) {
		log_error("Cannot update partial volume group %s.", vg->name);
		return 0;
	}

	if (vg_missing_pv_count(vg) && !vg->cmd->handles_missing_pvs) {
		log_error("Cannot update volume group %s while physical "
			  "volumes are missing.", vg->name);
		return 0;
	}

	if (vg_has_unknown_segments(vg) && !vg->cmd->handles_unknown_segments) {
		log_error("Cannot update volume group %s with unknown segments in it!",
			  vg->name);
		return 0;
	}

	if ((vg->fid->fmt->features & FMT_MDAS) && !_vg_adjust_ignored_mdas(vg))
		return_0;

	if (!vg_mda_used_count(vg)) {
		log_error("Aborting vg_write: No metadata areas to write to!");
		return 0;
	}

	if (!drop_cached_metadata(vg)) {
		log_error("Unable to drop cached metadata for VG %s.", vg->name);
		return 0;
	}

	if (critical_section())
		log_error(INTERNAL_ERROR
			  "Writing metadata in critical section.");

	/* Unlock memory if possible */
	memlock_unlock(vg->cmd);
	vg->seqno++;

        dm_list_iterate_items(pv_to_create, &vg->pvs_to_create) {
		if (!_pvcreate_write(vg->cmd, pv_to_create))
			return 0;
		pv_to_create->pv->status &= ~UNLABELLED_PV;
        }

	/* Write to each copy of the metadata area */
	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use) {
		if (!mda->ops->vg_write) {
			log_error("Format does not support writing volume"
				  "group metadata areas");
			/* Revert */
			dm_list_uniterate(mdah, &vg->fid->metadata_areas_in_use, &mda->list) {
				mda = dm_list_item(mdah, struct metadata_area);

				if (mda->ops->vg_revert &&
				    !mda->ops->vg_revert(vg->fid, vg, mda)) {
					stack;
				}
			}
			return 0;
		}
		if (!mda->ops->vg_write(vg->fid, vg, mda)) {
			stack;
			/* Revert */
			dm_list_uniterate(mdah, &vg->fid->metadata_areas_in_use, &mda->list) {
				mda = dm_list_item(mdah, struct metadata_area);

				if (mda->ops->vg_revert &&
				    !mda->ops->vg_revert(vg->fid, vg, mda)) {
					stack;
				}
			}
			return 0;
		}
	}

	/* Now pre-commit each copy of the new metadata */
	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use) {
		if (mda->ops->vg_precommit &&
		    !mda->ops->vg_precommit(vg->fid, vg, mda)) {
			stack;
			/* Revert */
			dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use) {
				if (mda->ops->vg_revert &&
				    !mda->ops->vg_revert(vg->fid, vg, mda)) {
					stack;
				}
			}
			return 0;
		}
	}

	return 1;
}

static int _vg_commit_mdas(struct volume_group *vg)
{
	struct metadata_area *mda, *tmda;
	struct dm_list ignored;
	int failed = 0;
	int cache_updated = 0;

	/* Rearrange the metadata_areas_in_use so ignored mdas come first. */
	dm_list_init(&ignored);
	dm_list_iterate_items_safe(mda, tmda, &vg->fid->metadata_areas_in_use)
		if (mda_is_ignored(mda))
			dm_list_move(&ignored, &mda->list);

	dm_list_iterate_items_safe(mda, tmda, &ignored)
		dm_list_move(&vg->fid->metadata_areas_in_use, &mda->list);

	/* Commit to each copy of the metadata area */
	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use) {
		failed = 0;
		if (mda->ops->vg_commit &&
		    !mda->ops->vg_commit(vg->fid, vg, mda)) {
			stack;
			failed = 1;
		}
		/* Update cache first time we succeed */
		if (!failed && !cache_updated) {
			lvmcache_update_vg(vg, 0);
			cache_updated = 1;
		}
	}
	return cache_updated;
}

/* Commit pending changes */
int vg_commit(struct volume_group *vg)
{
	int cache_updated = 0;

	if (!lvmcache_vgname_is_locked(vg->name)) {
		log_error(INTERNAL_ERROR "Attempt to write new VG metadata "
			  "without locking %s", vg->name);
		return cache_updated;
	}

	cache_updated = _vg_commit_mdas(vg);

	if (cache_updated) {
		/* Instruct remote nodes to upgrade cached metadata. */
		remote_commit_cached_metadata(vg);
		/*
		 * We need to clear old_name after a successful commit.
		 * The volume_group structure could be reused later.
		 */
		vg->old_name = NULL;
	}

	/* If update failed, remove any cached precommitted metadata. */
	if (!cache_updated && !drop_cached_metadata(vg))
		log_error("Attempt to drop cached metadata failed "
			  "after commit for VG %s.", vg->name);

	/* If at least one mda commit succeeded, it was committed */
	return cache_updated;
}

/* Don't commit any pending changes */
void vg_revert(struct volume_group *vg)
{
	struct metadata_area *mda;

	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use) {
		if (mda->ops->vg_revert &&
		    !mda->ops->vg_revert(vg->fid, vg, mda)) {
			stack;
		}
	}

	if (!drop_cached_metadata(vg))
		log_error("Attempt to drop cached metadata failed "
			  "after reverted update for VG %s.", vg->name);

	remote_revert_cached_metadata(vg);
}

struct _vg_read_orphan_baton {
	struct volume_group *vg;
	int warnings;
};

static int _vg_read_orphan_pv(struct lvmcache_info *info, void *baton)
{
	struct _vg_read_orphan_baton *b = baton;
	struct physical_volume *pv = NULL;
	struct pv_list *pvl;

	if (!(pv = _pv_read(b->vg->cmd, b->vg->vgmem, dev_name(lvmcache_device(info)),
			    b->vg->fid, b->warnings, 0))) {
		return 1;
	}
	if (!(pvl = dm_pool_zalloc(b->vg->vgmem, sizeof(*pvl)))) {
		log_error("pv_list allocation failed");
		free_pv_fid(pv);
		return 0;
	}
	pvl->pv = pv;
	add_pvl_to_vgs(b->vg, pvl);
	return 1;
}

/* Make orphan PVs look like a VG */
static struct volume_group *_vg_read_orphans(struct cmd_context *cmd,
					     int warnings,
					     const char *orphan_vgname)
{
	const struct format_type *fmt;
	struct lvmcache_vginfo *vginfo;
	struct volume_group *vg = NULL;
	struct _vg_read_orphan_baton baton;

	lvmcache_label_scan(cmd, 0);

	if (!(vginfo = lvmcache_vginfo_from_vgname(orphan_vgname, NULL)))
		return_NULL;

	if (!(fmt = lvmcache_fmt_from_vgname(orphan_vgname, NULL, 0)))
		return_NULL;

	vg = fmt->orphan_vg;

	baton.warnings = warnings;
	baton.vg = vg;
	lvmcache_foreach_pv(vginfo, _vg_read_orphan_pv, &baton);

	return vg;
}

static int _update_pv_list(struct dm_pool *pvmem, struct dm_list *all_pvs, struct volume_group *vg)
{
	struct pv_list *pvl, *pvl2;

	dm_list_iterate_items(pvl, &vg->pvs) {
		dm_list_iterate_items(pvl2, all_pvs) {
			if (pvl->pv->dev == pvl2->pv->dev)
				goto next_pv;
		}

		/*
		 * PV is not on list so add it.
		 */
		if (!(pvl2 = _copy_pvl(pvmem, pvl))) {
			log_error("pv_list allocation for '%s' failed",
				  pv_dev_name(pvl->pv));
			return 0;
		}
		dm_list_add(all_pvs, &pvl2->list);
  next_pv:
		;
	}

	return 1;
}

static void _free_pv_list(struct dm_list *all_pvs)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, all_pvs)
		pvl->pv->fid->fmt->ops->destroy_instance(pvl->pv->fid);
}

static void _destroy_fid(struct format_instance **fid)
{
	if (*fid) {
		(*fid)->fmt->ops->destroy_instance(*fid);
		*fid = NULL;
	}
}

int vg_missing_pv_count(const struct volume_group *vg)
{
	int ret = 0;
	struct pv_list *pvl;
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (is_missing_pv(pvl->pv))
			++ ret;
	}
	return ret;
}

static void check_reappeared_pv(struct volume_group *correct_vg,
				struct physical_volume *pv)
{
	struct pv_list *pvl;

        /*
         * Skip these checks in case the tool is going to deal with missing
         * PVs, especially since the resulting messages can be pretty
         * confusing.
         */
        if (correct_vg->cmd->handles_missing_pvs)
            return;

	dm_list_iterate_items(pvl, &correct_vg->pvs)
		if (pv->dev == pvl->pv->dev && is_missing_pv(pvl->pv)) {
			log_warn("Missing device %s reappeared, updating "
				 "metadata for VG %s to version %u.",
				 pv_dev_name(pvl->pv),  pv_vg_name(pvl->pv), 
				 correct_vg->seqno);
			if (pvl->pv->pe_alloc_count == 0) {
				pv->status &= ~MISSING_PV;
				pvl->pv->status &= ~MISSING_PV;
			} else
				log_warn("Device still marked missing because of allocated data "
					 "on it, remove volumes and consider vgreduce --removemissing.");
		}
}

static int _check_mda_in_use(struct metadata_area *mda, void *_in_use)
{
	int *in_use = _in_use;
	if (!mda_is_ignored(mda))
		*in_use = 1;
	return 1;
}

/* Caller sets consistent to 1 if it's safe for vg_read_internal to correct
 * inconsistent metadata on disk (i.e. the VG write lock is held).
 * This guarantees only consistent metadata is returned.
 * If consistent is 0, caller must check whether consistent == 1 on return
 * and take appropriate action if it isn't (e.g. abort; get write lock
 * and call vg_read_internal again).
 *
 * If precommitted is set, use precommitted metadata if present.
 *
 * Either of vgname or vgid may be NULL.
 *
 * Note: vginfo structs must not be held or used as parameters
 *       across the call to this function.
 */
static struct volume_group *_vg_read(struct cmd_context *cmd,
				     const char *vgname,
				     const char *vgid,
				     int warnings, 
				     int *consistent, unsigned precommitted)
{
	struct format_instance *fid = NULL;
	struct format_instance_ctx fic;
	const struct format_type *fmt;
	struct volume_group *vg, *correct_vg = NULL;
	struct metadata_area *mda;
	struct lvmcache_info *info;
	int inconsistent = 0;
	int inconsistent_vgid = 0;
	int inconsistent_pvs = 0;
	int inconsistent_mdas = 0;
	int inconsistent_mda_count = 0;
	unsigned use_precommitted = precommitted;
	unsigned saved_handles_missing_pvs = cmd->handles_missing_pvs;
	struct dm_list *pvids;
	struct pv_list *pvl, *pvl2;
	struct dm_list all_pvs;
	char uuid[64] __attribute__((aligned(8)));

	if (is_orphan_vg(vgname)) {
		if (use_precommitted) {
			log_error(INTERNAL_ERROR "vg_read_internal requires vgname "
				  "with pre-commit.");
			return NULL;
		}
		*consistent = 1;
		return _vg_read_orphans(cmd, warnings, vgname);
	}

	/*
	 * If cached metadata was inconsistent and *consistent is set
	 * then repair it now.  Otherwise just return it.
	 * Also return if use_precommitted is set due to the FIXME in
	 * the missing PV logic below.
	 */
	if ((correct_vg = lvmcache_get_vg(vgid, precommitted)) &&
	    (use_precommitted || !*consistent)) {
		*consistent = 1;
		return correct_vg;
	} else {
		release_vg(correct_vg);
		correct_vg = NULL;
	}

	/* Find the vgname in the cache */
	/* If it's not there we must do full scan to be completely sure */
	if (!(fmt = lvmcache_fmt_from_vgname(vgname, vgid, 1))) {
		lvmcache_label_scan(cmd, 0);
		if (!(fmt = lvmcache_fmt_from_vgname(vgname, vgid, 1))) {
			/* Independent MDAs aren't supported under low memory */
			if (!cmd->independent_metadata_areas && critical_section())
				return_NULL;
			lvmcache_label_scan(cmd, 2);
			if (!(fmt = lvmcache_fmt_from_vgname(vgname, vgid, 0)))
				return_NULL;
		}
	}

	/* Now determine the correct vgname if none was supplied */
	if (!vgname && !(vgname = lvmcache_vgname_from_vgid(cmd->mem, vgid)))
		return_NULL;

	if (use_precommitted && !(fmt->features & FMT_PRECOMMIT))
		use_precommitted = 0;

	/* create format instance with appropriate metadata area */
	fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = vgname;
	fic.context.vg_ref.vg_id = vgid;
	if (!(fid = fmt->ops->create_instance(fmt, &fic))) {
		log_error("Failed to create format instance");
		return NULL;
	}

	/* Store pvids for later so we can check if any are missing */
	if (!(pvids = lvmcache_get_pvids(cmd, vgname, vgid))) {
		_destroy_fid(&fid);
		return_NULL;
	}

	/*
	 * We use the fid globally here so prevent the release_vg
	 * call to destroy the fid - we may want to reuse it!
	 */
	fid->ref_count++;
	/* Ensure contents of all metadata areas match - else do recovery */
	inconsistent_mda_count=0;
	dm_list_iterate_items(mda, &fid->metadata_areas_in_use) {

		if ((use_precommitted &&
		     !(vg = mda->ops->vg_read_precommit(fid, vgname, mda))) ||
		    (!use_precommitted &&
		     !(vg = mda->ops->vg_read(fid, vgname, mda)))) {
			inconsistent = 1;
			release_vg(vg);
			continue;
		}

		if (!correct_vg) {
			correct_vg = vg;
			continue;
		}

		/* FIXME Also ensure contents same - checksum compare? */
		if (correct_vg->seqno != vg->seqno) {
			if (cmd->metadata_read_only)
				log_very_verbose("Not repairing VG %s metadata seqno (%d != %d) "
						  "as global/metadata_read_only is set.",
						  vgname, vg->seqno, correct_vg->seqno);
			else
				inconsistent = 1;

			if (vg->seqno > correct_vg->seqno) {
				release_vg(correct_vg);
				correct_vg = vg;
			} else {
				mda->status |= MDA_INCONSISTENT;
				++inconsistent_mda_count;
			}
		}

		if (vg != correct_vg)
			release_vg(vg);
	}
	fid->ref_count--;

	/* Ensure every PV in the VG was in the cache */
	if (correct_vg) {
		/*
		 * If the VG has PVs without mdas, or ignored mdas, they may
		 * still be orphans in the cache: update the cache state here,
		 * and update the metadata lists in the vg.
		 */
		if (!inconsistent &&
		    dm_list_size(&correct_vg->pvs) > dm_list_size(pvids)) {
			dm_list_iterate_items(pvl, &correct_vg->pvs) {
				if (!pvl->pv->dev) {
					inconsistent_pvs = 1;
					break;
				}

				if (str_list_match_item(pvids, pvl->pv->dev->pvid))
					continue;

				/*
				 * PV not marked as belonging to this VG in cache.
				 * Check it's an orphan without metadata area
				 * not ignored.
				 */
				if (!(info = lvmcache_info_from_pvid(pvl->pv->dev->pvid, 1)) ||
				    !lvmcache_is_orphan(info)) {
					inconsistent_pvs = 1;
					break;
				}
				if (lvmcache_mda_count(info)) {
					if (!lvmcache_fid_add_mdas_pv(info, fid)) {
						release_vg(correct_vg);
						return_NULL;
					}

					log_debug("Empty mda found for VG %s.", vgname);

					if (inconsistent_mdas)
						continue;

					/*
					 * If any newly-added mdas are in-use then their
					 * metadata needs updating.
					 */
					lvmcache_foreach_mda(info, _check_mda_in_use,
							     &inconsistent_mdas);
				}
			}

			/* If the check passed, let's update VG and recalculate pvids */
			if (!inconsistent_pvs) {
				log_debug("Updating cache for PVs without mdas "
					  "in VG %s.", vgname);
				/*
				 * If there is no precommitted metadata, committed metadata
				 * is read and stored in the cache even if use_precommitted is set
				 */
				lvmcache_update_vg(correct_vg, correct_vg->status & PRECOMMITTED);

				if (!(pvids = lvmcache_get_pvids(cmd, vgname, vgid))) {
					release_vg(correct_vg);
					return_NULL;
				}
			}
		}

		fid->ref_count++;
		if (dm_list_size(&correct_vg->pvs) !=
		    dm_list_size(pvids) + vg_missing_pv_count(correct_vg)) {
			log_debug("Cached VG %s had incorrect PV list",
				  vgname);

			if (critical_section())
				inconsistent = 1;
			else {
				release_vg(correct_vg);
				correct_vg = NULL;
			}
		} else dm_list_iterate_items(pvl, &correct_vg->pvs) {
			if (is_missing_pv(pvl->pv))
				continue;
			if (!str_list_match_item(pvids, pvl->pv->dev->pvid)) {
				log_debug("Cached VG %s had incorrect PV list",
					  vgname);
				release_vg(correct_vg);
				correct_vg = NULL;
				break;
			}
		}

		if (correct_vg && inconsistent_mdas) {
			release_vg(correct_vg);
			correct_vg = NULL;
		}
		fid->ref_count--;
	}

	dm_list_init(&all_pvs);

	/* Failed to find VG where we expected it - full scan and retry */
	if (!correct_vg) {
		/*
		 * Free outstanding format instance that remained unassigned
		 * from previous step where we tried to get the "correct_vg",
		 * but we failed to do so (so there's a dangling fid now).
		 */
		_destroy_fid(&fid);

		inconsistent = 0;

		/* Independent MDAs aren't supported under low memory */
		if (!cmd->independent_metadata_areas && critical_section())
			return_NULL;
		lvmcache_label_scan(cmd, 2);
		if (!(fmt = lvmcache_fmt_from_vgname(vgname, vgid, 0)))
			return_NULL;

		if (precommitted && !(fmt->features & FMT_PRECOMMIT))
			use_precommitted = 0;

		/* create format instance with appropriate metadata area */
		fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
		fic.context.vg_ref.vg_name = vgname;
		fic.context.vg_ref.vg_id = vgid;
		if (!(fid = fmt->ops->create_instance(fmt, &fic))) {
			log_error("Failed to create format instance");
			return NULL;
		}

		/*
		 * We use the fid globally here so prevent the release_vg
		 * call to destroy the fid - we may want to reuse it!
		*/
		fid->ref_count++;
		/* Ensure contents of all metadata areas match - else recover */
		inconsistent_mda_count=0;
		dm_list_iterate_items(mda, &fid->metadata_areas_in_use) {
			if ((use_precommitted &&
			     !(vg = mda->ops->vg_read_precommit(fid, vgname,
								mda))) ||
			    (!use_precommitted &&
			     !(vg = mda->ops->vg_read(fid, vgname, mda)))) {
				inconsistent = 1;
				continue;
			}
			if (!correct_vg) {
				correct_vg = vg;
				if (!_update_pv_list(cmd->mem, &all_pvs, correct_vg)) {
					_free_pv_list(&all_pvs);
					fid->ref_count--;
					release_vg(vg);
					return_NULL;
				}
				continue;
			}

			if (!id_equal(&vg->id, &correct_vg->id)) {
				inconsistent = 1;
				inconsistent_vgid = 1;
			}

			/* FIXME Also ensure contents same - checksums same? */
			if (correct_vg->seqno != vg->seqno) {
				/* Ignore inconsistent seqno if told to skip repair logic */
				if (cmd->metadata_read_only)
					log_very_verbose("Not repairing VG %s metadata seqno (%d != %d) "
							  "as global/metadata_read_only is set.",
							  vgname, vg->seqno, correct_vg->seqno);
				else
					inconsistent = 1;

				if (!_update_pv_list(cmd->mem, &all_pvs, vg)) {
					_free_pv_list(&all_pvs);
					fid->ref_count--;
					release_vg(vg);
					release_vg(correct_vg);
					return_NULL;
				}
				if (vg->seqno > correct_vg->seqno) {
					release_vg(correct_vg);
					correct_vg = vg;
				} else {
					mda->status |= MDA_INCONSISTENT;
					++inconsistent_mda_count;
				}
			}

			if (vg != correct_vg)
				release_vg(vg);
		}
		fid->ref_count--;

		/* Give up looking */
		if (!correct_vg) {
			_free_pv_list(&all_pvs);
			_destroy_fid(&fid);
			return_NULL;
		}
	}

	/*
	 * If there is no precommitted metadata, committed metadata
	 * is read and stored in the cache even if use_precommitted is set
	 */
	lvmcache_update_vg(correct_vg, (correct_vg->status & PRECOMMITTED));

	if (inconsistent) {
		/* FIXME Test should be if we're *using* precommitted metadata not if we were searching for it */
		if (use_precommitted) {
			log_error("Inconsistent pre-commit metadata copies "
				  "for volume group %s", vgname);

			/*
			 * Check whether all of the inconsistent MDAs were on
			 * MISSING PVs -- in that case, we should be safe.
			 */
			dm_list_iterate_items(mda, &fid->metadata_areas_in_use) {
				if (mda->status & MDA_INCONSISTENT) {
					log_debug("Checking inconsistent MDA: %s", dev_name(mda_get_device(mda)));
					dm_list_iterate_items(pvl, &correct_vg->pvs) {
						if (mda_get_device(mda) == pvl->pv->dev &&
						    (pvl->pv->status & MISSING_PV))
							--inconsistent_mda_count;
					}
				}
			}

			if (inconsistent_mda_count < 0)
				log_error(INTERNAL_ERROR "Too many inconsistent MDAs.");

			if (!inconsistent_mda_count) {
				*consistent = 0;
				_free_pv_list(&all_pvs);
				return correct_vg;
			}
			_free_pv_list(&all_pvs);
			release_vg(correct_vg);
			return NULL;
		}

		if (!*consistent) {
			_free_pv_list(&all_pvs);
			return correct_vg;
		}

		/* Don't touch if vgids didn't match */
		if (inconsistent_vgid) {
			log_error("Inconsistent metadata UUIDs found for "
				  "volume group %s", vgname);
			*consistent = 0;
			_free_pv_list(&all_pvs);
			return correct_vg;
		}

		log_warn("WARNING: Inconsistent metadata found for VG %s - updating "
			 "to use version %u", vgname, correct_vg->seqno);

		/*
		 * If PV is marked missing but we found it,
		 * update metadata and remove MISSING flag
		 */
		dm_list_iterate_items(pvl, &all_pvs)
			check_reappeared_pv(correct_vg, pvl->pv);

		cmd->handles_missing_pvs = 1;
		if (!vg_write(correct_vg)) {
			log_error("Automatic metadata correction failed");
			_free_pv_list(&all_pvs);
			release_vg(correct_vg);
			cmd->handles_missing_pvs = saved_handles_missing_pvs;
			return NULL;
		}
		cmd->handles_missing_pvs = saved_handles_missing_pvs;

		if (!vg_commit(correct_vg)) {
			log_error("Automatic metadata correction commit "
				  "failed");
			release_vg(correct_vg);
			return NULL;
		}

		dm_list_iterate_items(pvl, &all_pvs) {
			dm_list_iterate_items(pvl2, &correct_vg->pvs) {
				if (pvl->pv->dev == pvl2->pv->dev)
					goto next_pv;
			}
			if (!id_write_format(&pvl->pv->id, uuid, sizeof(uuid))) {
				_free_pv_list(&all_pvs);
				release_vg(correct_vg);
				return_NULL;
			}
			log_error("Removing PV %s (%s) that no longer belongs to VG %s",
				  pv_dev_name(pvl->pv), uuid, correct_vg->name);
			if (!pv_write_orphan(cmd, pvl->pv)) {
				_free_pv_list(&all_pvs);
				release_vg(correct_vg);
				return_NULL;
			}

			/* Refresh metadata after orphan write */
			drop_cached_metadata(correct_vg);
      next_pv:
			;
		}
	}

	_free_pv_list(&all_pvs);

	if (vg_missing_pv_count(correct_vg)) {
		log_verbose("There are %d physical volumes missing.",
			    vg_missing_pv_count(correct_vg));
		vg_mark_partial_lvs(correct_vg, 1);
	}

	if ((correct_vg->status & PVMOVE) && !pvmove_mode()) {
		log_error("WARNING: Interrupted pvmove detected in "
			  "volume group %s", correct_vg->name);
		log_error("Please restore the metadata by running "
			  "vgcfgrestore.");
		release_vg(correct_vg);
		return NULL;
	}

	*consistent = 1;
	return correct_vg;
}

struct volume_group *vg_read_internal(struct cmd_context *cmd, const char *vgname,
			     const char *vgid, int warnings, int *consistent)
{
	struct volume_group *vg;
	struct lv_list *lvl;

	if (!(vg = _vg_read(cmd, vgname, vgid, warnings, consistent, 0)))
		return NULL;

	if (!check_pv_segments(vg)) {
		log_error(INTERNAL_ERROR "PV segments corrupted in %s.",
			  vg->name);
		release_vg(vg);
		return NULL;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (!check_lv_segments(lvl->lv, 0)) {
			log_error(INTERNAL_ERROR "LV segments corrupted in %s.",
				  lvl->lv->name);
			release_vg(vg);
			return NULL;
		}
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		/*
		 * Checks that cross-reference other LVs.
		 */
		if (!check_lv_segments(lvl->lv, 1)) {
			log_error(INTERNAL_ERROR "LV segments corrupted in %s.",
				  lvl->lv->name);
			release_vg(vg);
			return NULL;
		}
	}

	return vg;
}

void free_pv_fid(struct physical_volume *pv)
{
	if (!pv)
		return;

	if (pv->fid)
		pv->fid->fmt->ops->destroy_instance(pv->fid);
}

/* This is only called by lv_from_lvid, which is only called from
 * activate.c so we know the appropriate VG lock is already held and
 * the vg_read_internal is therefore safe.
 */
static struct volume_group *_vg_read_by_vgid(struct cmd_context *cmd,
					    const char *vgid,
					    unsigned precommitted)
{
	const char *vgname;
	struct dm_list *vgnames;
	struct volume_group *vg;
	struct str_list *strl;
	int consistent = 0;

	/* Is corresponding vgname already cached? */
	if (lvmcache_vgid_is_cached(vgid)) {
		if ((vg = _vg_read(cmd, NULL, vgid, 1,
				   &consistent, precommitted)) &&
		    id_equal(&vg->id, (const struct id *)vgid)) {
			if (!consistent)
				log_error("Volume group %s metadata is "
					  "inconsistent", vg->name);
			return vg;
		}
		release_vg(vg);
	}

	/* Mustn't scan if memory locked: ensure cache gets pre-populated! */
	if (critical_section())
		return_NULL;

	/* FIXME Need a genuine read by ID here - don't vg_read_internal by name! */
	/* FIXME Disabled vgrenames while active for now because we aren't
	 *       allowed to do a full scan here any more. */

	// The slow way - full scan required to cope with vgrename
	lvmcache_label_scan(cmd, 2);
	if (!(vgnames = get_vgnames(cmd, 0))) {
		log_error("vg_read_by_vgid: get_vgnames failed");
		return NULL;
	}

	dm_list_iterate_items(strl, vgnames) {
		vgname = strl->str;
		if (!vgname)
			continue;	// FIXME Unnecessary?
		consistent = 0;
		if ((vg = _vg_read(cmd, vgname, vgid, 1, &consistent,
				   precommitted)) &&
		    id_equal(&vg->id, (const struct id *)vgid)) {
			if (!consistent) {
				log_error("Volume group %s metadata is "
					  "inconsistent", vgname);
				release_vg(vg);
				return NULL;
			}
			return vg;
		}
		release_vg(vg);
	}

	return NULL;
}

/* Only called by activate.c */
struct logical_volume *lv_from_lvid(struct cmd_context *cmd, const char *lvid_s,
				    unsigned precommitted)
{
	struct lv_list *lvl;
	struct volume_group *vg;
	const union lvid *lvid;

	lvid = (const union lvid *) lvid_s;

	log_very_verbose("Finding volume group for uuid %s", lvid_s);
	if (!(vg = _vg_read_by_vgid(cmd, (const char *)lvid->id[0].uuid, precommitted))) {
		log_error("Volume group for uuid not found: %s", lvid_s);
		return NULL;
	}

	log_verbose("Found volume group \"%s\"", vg->name);
	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg->name);
		goto out;
	}
	if (!(lvl = find_lv_in_vg_by_lvid(vg, lvid))) {
		log_very_verbose("Can't find logical volume id %s", lvid_s);
		goto out;
	}

	return lvl->lv;
out:
	release_vg(vg);
	return NULL;
}


const char *find_vgname_from_pvid(struct cmd_context *cmd,
				  const char *pvid)
{
	char *vgname;
	struct lvmcache_info *info;

	vgname = lvmcache_vgname_from_pvid(cmd, pvid);

	if (is_orphan_vg(vgname)) {
		if (!(info = lvmcache_info_from_pvid(pvid, 0))) {
			return_NULL;
		}
		/*
		 * If an orphan PV has no MDAs, or it has MDAs but the
		 * MDA is ignored, it may appear to be an orphan until
		 * the metadata is read off another PV in the same VG.
		 * Detecting this means checking every VG by scanning
		 * every PV on the system.
		 */
		if (lvmcache_uncertain_ownership(info)) {
			if (!scan_vgs_for_pvs(cmd, 1)) {
				log_error("Rescan for PVs without "
					  "metadata areas failed.");
				return NULL;
			}
			/*
			 * Ask lvmcache again - we may have a non-orphan
			 * name now
			 */
			vgname = lvmcache_vgname_from_pvid(cmd, pvid);
		}
	}
	return vgname;
}


const char *find_vgname_from_pvname(struct cmd_context *cmd,
				    const char *pvname)
{
	const char *pvid;

	pvid = lvmcache_pvid_from_devname(cmd, pvname);
	if (!pvid)
		/* Not a PV */
		return NULL;

	return find_vgname_from_pvid(cmd, pvid);
}

/**
 * pv_read - read and return a handle to a physical volume
 * @cmd: LVM command initiating the pv_read
 * @pv_name: full device name of the PV, including the path
 * @mdas: list of metadata areas of the PV
 * @label_sector: sector number where the PV label is stored on @pv_name
 * @warnings:
 *
 * Returns:
 *   PV handle - valid pv_name and successful read of the PV, or
 *   NULL - invalid parameter or error in reading the PV
 *
 * Note:
 *   FIXME - liblvm todo - make into function that returns handle
 */
struct physical_volume *pv_read(struct cmd_context *cmd, const char *pv_name,
				int warnings,
				int scan_label_only)
{
	return _pv_read(cmd, cmd->mem, pv_name, NULL, warnings, scan_label_only);
}

/* FIXME Use label functions instead of PV functions */
static struct physical_volume *_pv_read(struct cmd_context *cmd,
					struct dm_pool *pvmem,
					const char *pv_name,
					struct format_instance *fid,
					int warnings, int scan_label_only)
{
	struct physical_volume *pv;
	struct label *label;
	struct lvmcache_info *info;
	struct device *dev;
	const struct format_type *fmt;

	if (!(dev = dev_cache_get(pv_name, cmd->filter)))
		return_NULL;

	if (!(label_read(dev, &label, UINT64_C(0)))) {
		if (warnings)
			log_error("No physical volume label read from %s",
				  pv_name);
		return NULL;
	}

	info = (struct lvmcache_info *) label->info;
	fmt = lvmcache_fmt(info);

	pv = _alloc_pv(pvmem, dev);
	if (!pv) {
		log_error("pv allocation for '%s' failed", pv_name);
		return NULL;
	}

	pv->label_sector = label->sector;

	/* FIXME Move more common code up here */
	if (!(lvmcache_fmt(info)->ops->pv_read(lvmcache_fmt(info), pv_name, pv, scan_label_only))) {
		log_error("Failed to read existing physical volume '%s'",
			  pv_name);
		goto bad;
	}

	if (!pv->size)
		goto bad;

	if (!alloc_pv_segment_whole_pv(pvmem, pv))
		goto_bad;

	if (fid)
		lvmcache_fid_add_mdas(info, fid, (const char *) &pv->id, ID_LEN);
	else {
		lvmcache_fid_add_mdas(info, fmt->orphan_vg->fid, (const char *) &pv->id, ID_LEN);
		pv_set_fid(pv, fmt->orphan_vg->fid);
	}

	return pv;
bad:
	free_pv_fid(pv);
	dm_pool_free(pvmem, pv);
	return NULL;
}

/* May return empty list */
struct dm_list *get_vgnames(struct cmd_context *cmd, int include_internal)
{
	return lvmcache_get_vgnames(cmd, include_internal);
}

struct dm_list *get_vgids(struct cmd_context *cmd, int include_internal)
{
	return lvmcache_get_vgids(cmd, include_internal);
}

static int _get_pvs(struct cmd_context *cmd, int warnings, struct dm_list **pvslist)
{
	struct str_list *strl;
	struct dm_list * uninitialized_var(results);
	const char *vgname, *vgid;
	struct pv_list *pvl, *pvl_copy;
	struct dm_list *vgids;
	struct volume_group *vg;
	int consistent = 0;
	int old_pvmove;

	lvmcache_label_scan(cmd, 0);

	if (pvslist) {
		if (!(results = dm_pool_alloc(cmd->mem, sizeof(*results)))) {
			log_error("PV list allocation failed");
			return 0;
		}

		dm_list_init(results);
	}

	/* Get list of VGs */
	if (!(vgids = get_vgids(cmd, 1))) {
		log_error("get_pvs: get_vgids failed");
		return 0;
	}

	/* Read every VG to ensure cache consistency */
	/* Orphan VG is last on list */
	old_pvmove = pvmove_mode();
	init_pvmove(1);
	dm_list_iterate_items(strl, vgids) {
		vgid = strl->str;
		if (!vgid)
			continue;	/* FIXME Unnecessary? */
		consistent = 0;
		if (!(vgname = lvmcache_vgname_from_vgid(NULL, vgid))) {
			stack;
			continue;
		}
		if (!(vg = vg_read_internal(cmd, vgname, vgid, warnings, &consistent))) {
			stack;
			continue;
		}
		if (!consistent)
			log_warn("WARNING: Volume Group %s is not consistent",
				 vgname);

		/* Move PVs onto results list */
		if (pvslist)
			dm_list_iterate_items(pvl, &vg->pvs) {
				if (!(pvl_copy = _copy_pvl(cmd->mem, pvl))) {
					log_error("PV list allocation failed");
					release_vg(vg);
					return 0;
				}
				dm_list_add(results, &pvl_copy->list);
			}
		release_vg(vg);
	}
	init_pvmove(old_pvmove);

	if (pvslist)
		*pvslist = results;
	else
		dm_pool_free(cmd->mem, vgids);

	return 1;
}

struct dm_list *get_pvs(struct cmd_context *cmd)
{
	struct dm_list *results;

	if (!_get_pvs(cmd, 1, &results))
		return NULL;

	return results;
}

int scan_vgs_for_pvs(struct cmd_context *cmd, int warnings)
{
	return _get_pvs(cmd, warnings, NULL);
}

int pv_write(struct cmd_context *cmd __attribute__((unused)),
	     struct physical_volume *pv, int allow_non_orphan)
{
	if (!pv->fmt->ops->pv_write) {
		log_error("Format does not support writing physical volumes");
		return 0;
	}

	/*
	 * FIXME: Try to remove this restriction. This requires checking
	 *        that the PV and the VG are in a consistent state. We need
	 *        to provide some revert mechanism since PV label together
	 *        with VG metadata write is not atomic.
	 */
	if (!allow_non_orphan &&
	    (!is_orphan_vg(pv->vg_name) || pv->pe_alloc_count)) {
		log_error("Assertion failed: can't _pv_write non-orphan PV "
			  "(in VG %s)", pv->vg_name);
		return 0;
	}

	if (!pv->fmt->ops->pv_write(pv->fmt, pv))
		return_0;

	return 1;
}

int pv_write_orphan(struct cmd_context *cmd, struct physical_volume *pv)
{
	const char *old_vg_name = pv->vg_name;

	pv->vg_name = cmd->fmt->orphan_vg_name;
	pv->status = ALLOCATABLE_PV;
	pv->pe_alloc_count = 0;

	if (!dev_get_size(pv->dev, &pv->size)) {
		log_error("%s: Couldn't get size.", pv_dev_name(pv));
		return 0;
	}

	if (!pv_write(cmd, pv, 0)) {
		log_error("Failed to clear metadata from physical "
			  "volume \"%s\" after removal from \"%s\"",
			  pv_dev_name(pv), old_vg_name);
		return 0;
	}

	return 1;
}

int is_global_vg(const char *vg_name)
{
	return (vg_name && !strcmp(vg_name, VG_GLOBAL)) ? 1 : 0;
}

/**
 * is_orphan_vg - Determine whether a vg_name is an orphan
 * @vg_name: pointer to the vg_name
 */
int is_orphan_vg(const char *vg_name)
{
	return (vg_name && !strncmp(vg_name, ORPHAN_PREFIX, sizeof(ORPHAN_PREFIX) - 1)) ? 1 : 0;
}

/*
 * Exclude pseudo VG names used for locking.
 */
int is_real_vg(const char *vg_name)
{
	return (vg_name && *vg_name != '#');
}

static int _analyze_mda(struct metadata_area *mda, void *baton)
{
	const struct format_type *fmt = baton;
	mda->ops->pv_analyze_mda(fmt, mda);
	return 1;
}

/*
 * Returns:
 *  0 - fail
 *  1 - success
 */
int pv_analyze(struct cmd_context *cmd, const char *pv_name,
	       uint64_t label_sector)
{
	struct label *label;
	struct device *dev;
	struct lvmcache_info *info;

	dev = dev_cache_get(pv_name, cmd->filter);
	if (!dev) {
		log_error("Device %s not found (or ignored by filtering).",
			  pv_name);
		return 0;
	}

	/*
	 * First, scan for LVM labels.
	 */
	if (!label_read(dev, &label, label_sector)) {
		log_error("Could not find LVM label on %s",
			  pv_name);
		return 0;
	}

	log_print("Found label on %s, sector %"PRIu64", type=%.8s",
		  pv_name, label->sector, label->type);

	/*
	 * Next, loop through metadata areas
	 */
	info = label->info;
	lvmcache_foreach_mda(info, _analyze_mda, (void *)lvmcache_fmt(info));

	return 1;
}

/* FIXME: remove / combine this with locking? */
int vg_check_write_mode(struct volume_group *vg)
{
	if (vg->open_mode != 'w') {
		log_errno(EPERM, "Attempt to modify a read-only VG");
		return 0;
	}
	return 1;
}

/*
 * Performs a set of checks against a VG according to bits set in status
 * and returns FAILED_* bits for those that aren't acceptable.
 *
 * FIXME Remove the unnecessary duplicate definitions and return bits directly.
 */
static uint32_t _vg_bad_status_bits(const struct volume_group *vg,
				    uint64_t status)
{
	uint32_t failure = 0;

	if ((status & CLUSTERED) &&
	    (vg_is_clustered(vg)) && !locking_is_clustered()) {
		log_error("Skipping clustered volume group %s", vg->name);
		/* Return because other flags are considered undefined. */
		return FAILED_CLUSTERED;
	}

	if ((status & EXPORTED_VG) &&
	    vg_is_exported(vg)) {
		log_error("Volume group %s is exported", vg->name);
		failure |= FAILED_EXPORTED;
	}

	if ((status & LVM_WRITE) &&
	    !(vg->status & LVM_WRITE)) {
		log_error("Volume group %s is read-only", vg->name);
		failure |= FAILED_READ_ONLY;
	}

	if ((status & RESIZEABLE_VG) &&
	    !vg_is_resizeable(vg)) {
		log_error("Volume group %s is not resizeable.", vg->name);
		failure |= FAILED_RESIZEABLE;
	}

	return failure;
}

/**
 * vg_check_status - check volume group status flags and log error
 * @vg - volume group to check status flags
 * @status - specific status flags to check (e.g. EXPORTED_VG)
 */
int vg_check_status(const struct volume_group *vg, uint64_t status)
{
	return !_vg_bad_status_bits(vg, status);
}

static struct volume_group *_recover_vg(struct cmd_context *cmd,
			 const char *vg_name, const char *vgid)
{
	int consistent = 1;
	struct volume_group *vg;

	unlock_vg(cmd, vg_name);

	dev_close_all();

	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE))
		return_NULL;

	if (!(vg = vg_read_internal(cmd, vg_name, vgid, 1, &consistent)))
		return_NULL;

	if (!consistent) {
		release_vg(vg);
		return_NULL;
	}

	return (struct volume_group *)vg;
}

/*
 * Consolidated locking, reading, and status flag checking.
 *
 * If the metadata is inconsistent, setting READ_ALLOW_INCONSISTENT in
 * misc_flags will return it with FAILED_INCONSISTENT set instead of 
 * giving you nothing.
 *
 * Use vg_read_error(vg) to determine the result.  Nonzero means there were
 * problems reading the volume group.
 * Zero value means that the VG is open and appropriate locks are held.
 */
static struct volume_group *_vg_lock_and_read(struct cmd_context *cmd, const char *vg_name,
			       const char *vgid, uint32_t lock_flags,
			       uint64_t status_flags, uint32_t misc_flags)
{
	struct volume_group *vg = NULL;
 	int consistent = 1;
	int consistent_in;
	uint32_t failure = 0;
	int already_locked;

	if (misc_flags & READ_ALLOW_INCONSISTENT || lock_flags != LCK_VG_WRITE)
		consistent = 0;

	if (!validate_name(vg_name) && !is_orphan_vg(vg_name)) {
		log_error("Volume group name %s has invalid characters",
			  vg_name);
		return NULL;
	}

	already_locked = lvmcache_vgname_is_locked(vg_name);

	if (!already_locked && !(misc_flags & READ_WITHOUT_LOCK) &&
	    !lock_vol(cmd, vg_name, lock_flags)) {
		log_error("Can't get lock for %s", vg_name);
		return _vg_make_handle(cmd, vg, FAILED_LOCKING);
	}

	if (is_orphan_vg(vg_name))
		status_flags &= ~LVM_WRITE;

	consistent_in = consistent;

	/* If consistent == 1, we get NULL here if correction fails. */
	if (!(vg = vg_read_internal(cmd, vg_name, vgid, 1, &consistent))) {
		if (consistent_in && !consistent) {
			log_error("Volume group \"%s\" inconsistent.", vg_name);
			failure |= FAILED_INCONSISTENT;
			goto_bad;
		}

		log_error("Volume group \"%s\" not found", vg_name);

		failure |= FAILED_NOTFOUND;
		goto_bad;
	}

	if (vg_is_clustered(vg) && !locking_is_clustered()) {
		log_error("Skipping clustered volume group %s", vg->name);
		failure |= FAILED_CLUSTERED;
		goto_bad;
	}

	/* consistent == 0 when VG is not found, but failed == FAILED_NOTFOUND */
	if (!consistent && !failure) {
		release_vg(vg);
		if (!(vg = _recover_vg(cmd, vg_name, vgid))) {
			log_error("Recovery of volume group \"%s\" failed.",
				  vg_name);
			failure |= FAILED_INCONSISTENT;
			goto_bad;
		}
	}

	/*
	 * Check that the tool can handle tricky cases -- missing PVs and
	 * unknown segment types.
	 */

	if (!cmd->handles_missing_pvs && vg_missing_pv_count(vg) &&
	    lock_flags == LCK_VG_WRITE) {
		log_error("Cannot change VG %s while PVs are missing.", vg->name);
		log_error("Consider vgreduce --removemissing.");
		failure |= FAILED_INCONSISTENT; /* FIXME new failure code here? */
		goto_bad;
	}

	if (!cmd->handles_unknown_segments && vg_has_unknown_segments(vg) &&
	    lock_flags == LCK_VG_WRITE) {
		log_error("Cannot change VG %s with unknown segments in it!",
			  vg->name);
		failure |= FAILED_INCONSISTENT; /* FIXME new failure code here? */
		goto_bad;
	}

	failure |= _vg_bad_status_bits(vg, status_flags);
	if (failure)
		goto_bad;

	return _vg_make_handle(cmd, vg, failure);

bad:
	if (!already_locked && !(misc_flags & READ_WITHOUT_LOCK))
		unlock_vg(cmd, vg_name);

	return _vg_make_handle(cmd, vg, failure);
}

/*
 * vg_read: High-level volume group metadata read function.
 *
 * vg_read_error() must be used on any handle returned to check for errors.
 *
 *  - metadata inconsistent and automatic correction failed: FAILED_INCONSISTENT
 *  - VG is read-only: FAILED_READ_ONLY
 *  - VG is EXPORTED, unless flags has READ_ALLOW_EXPORTED: FAILED_EXPORTED
 *  - VG is not RESIZEABLE: FAILED_RESIZEABLE
 *  - locking failed: FAILED_LOCKING
 *
 * On failures, all locks are released, unless one of the following applies:
 *  - vgname_is_locked(lock_name) is true
 * FIXME: remove the above 2 conditions if possible and make an error always
 * release the lock.
 *
 * Volume groups are opened read-only unless flags contains READ_FOR_UPDATE.
 *
 * Checking for VG existence:
 *
 * FIXME: We want vg_read to attempt automatic recovery after acquiring a
 * temporary write lock: if that fails, we bail out as usual, with failed &
 * FAILED_INCONSISTENT. If it works, we are good to go. Code that's been in
 * toollib just set lock_flags to LCK_VG_WRITE and called vg_read_internal with
 * *consistent = 1.
 */
struct volume_group *vg_read(struct cmd_context *cmd, const char *vg_name,
	      const char *vgid, uint32_t flags)
{
	uint64_t status = UINT64_C(0);
	uint32_t lock_flags = LCK_VG_READ;

	if (flags & READ_FOR_UPDATE) {
		status |= EXPORTED_VG | LVM_WRITE;
		lock_flags = LCK_VG_WRITE;
	}

	if (flags & READ_ALLOW_EXPORTED)
		status &= ~EXPORTED_VG;

	return _vg_lock_and_read(cmd, vg_name, vgid, lock_flags, status, flags);
}

/*
 * A high-level volume group metadata reading function. Open a volume group for
 * later update (this means the user code can change the metadata and later
 * request the new metadata to be written and committed).
 */
struct volume_group *vg_read_for_update(struct cmd_context *cmd, const char *vg_name,
			 const char *vgid, uint32_t flags)
{
	return vg_read(cmd, vg_name, vgid, flags | READ_FOR_UPDATE);
}

/*
 * Test the validity of a VG handle returned by vg_read() or vg_read_for_update().
 */
uint32_t vg_read_error(struct volume_group *vg_handle)
{
	if (!vg_handle)
		return FAILED_ALLOCATION;

	return vg_handle->read_status;
}

/*
 * Lock a vgname and/or check for existence.
 * Takes a WRITE lock on the vgname before scanning.
 * If scanning fails or vgname found, release the lock.
 * NOTE: If you find the return codes confusing, you might think of this
 * function as similar to an open() call with O_CREAT and O_EXCL flags
 * (open returns fail with -EEXIST if file already exists).
 *
 * Returns:
 * FAILED_LOCKING - Cannot lock name
 * FAILED_EXIST - VG name already exists - cannot reserve
 * SUCCESS - VG name does not exist in system and WRITE lock held
 */
uint32_t vg_lock_newname(struct cmd_context *cmd, const char *vgname)
{
	if (!lock_vol(cmd, vgname, LCK_VG_WRITE)) {
		return FAILED_LOCKING;
	}

	/* Find the vgname in the cache */
	/* If it's not there we must do full scan to be completely sure */
	if (!lvmcache_fmt_from_vgname(vgname, NULL, 1)) {
		lvmcache_label_scan(cmd, 0);
		if (!lvmcache_fmt_from_vgname(vgname, NULL, 1)) {
			/* Independent MDAs aren't supported under low memory */
			if (!cmd->independent_metadata_areas && critical_section()) {
				/*
				 * FIXME: Disallow calling this function if
				 * critical_section() is true.
				 */
				unlock_vg(cmd, vgname);
				return FAILED_LOCKING;
			}
			lvmcache_label_scan(cmd, 2);
			if (!lvmcache_fmt_from_vgname(vgname, NULL, 0)) {
				/* vgname not found after scanning */
				return SUCCESS;
			}
		}
	}

	/* Found vgname so cannot reserve. */
	unlock_vg(cmd, vgname);
	return FAILED_EXIST;
}

struct format_instance *alloc_fid(const struct format_type *fmt,
				  const struct format_instance_ctx *fic)
{
	struct dm_pool *mem;
	struct format_instance *fid;

	if (!(mem = dm_pool_create("format_instance", 1024)))
		return_NULL;

	if (!(fid = dm_pool_zalloc(mem, sizeof(*fid)))) {
		log_error("Couldn't allocate format_instance object.");
		goto bad;
	}

	fid->ref_count = 1;
	fid->mem = mem;
	fid->type = fic->type;
	fid->fmt = fmt;

	dm_list_init(&fid->metadata_areas_in_use);
	dm_list_init(&fid->metadata_areas_ignored);

	return fid;

bad:
	dm_pool_destroy(mem);
	return NULL;
}

void pv_set_fid(struct physical_volume *pv,
		struct format_instance *fid)
{
	if (fid)
		fid->ref_count++;

	if (pv->fid)
		pv->fid->fmt->ops->destroy_instance(pv->fid);

	pv->fid = fid;
}

void vg_set_fid(struct volume_group *vg,
		 struct format_instance *fid)
{
	struct pv_list *pvl;

	if (fid)
		fid->ref_count++;

	dm_list_iterate_items(pvl, &vg->pvs)
		pv_set_fid(pvl->pv, fid);

	dm_list_iterate_items(pvl, &vg->removed_pvs)
		pv_set_fid(pvl->pv, fid);

	if (vg->fid)
		vg->fid->fmt->ops->destroy_instance(vg->fid);

	vg->fid = fid;
}

static int _convert_key_to_string(const char *key, size_t key_len,
				  unsigned sub_key, char *buf, size_t buf_len)
{
	memcpy(buf, key, key_len);
	buf += key_len;
	buf_len -= key_len;
	if ((dm_snprintf(buf, buf_len, "_%u", sub_key) == -1))
		return_0;

	return 1;
}

int fid_add_mda(struct format_instance *fid, struct metadata_area *mda,
		 const char *key, size_t key_len, const unsigned sub_key)
{
	static char full_key[PATH_MAX];
	dm_list_add(mda_is_ignored(mda) ? &fid->metadata_areas_ignored :
					  &fid->metadata_areas_in_use, &mda->list);

	/* Return if the mda is not supposed to be indexed. */
	if (!key)
		return 1;

	/* Add metadata area to index. */
	if (!_convert_key_to_string(key, key_len, sub_key,
				    full_key, sizeof(full_key)))
	return_0;

	dm_hash_insert(fid->metadata_areas_index,
		       full_key, mda);

	return 1;
}

int fid_add_mdas(struct format_instance *fid, struct dm_list *mdas,
		 const char *key, size_t key_len)
{
	struct metadata_area *mda, *mda_new;
	unsigned mda_index = 0;

	dm_list_iterate_items(mda, mdas) {
		mda_new = mda_copy(fid->mem, mda);
		if (!mda_new)
			return_0;
		fid_remove_mda(fid, NULL, key, key_len, mda_index);
		fid_add_mda(fid, mda_new, key, key_len, mda_index);
		mda_index++;
	}

	return 1;
}

struct metadata_area *fid_get_mda_indexed(struct format_instance *fid,
					  const char *key, size_t key_len,
					  const unsigned sub_key)
{
	static char full_key[PATH_MAX];
	struct metadata_area *mda = NULL;


	if (!_convert_key_to_string(key, key_len, sub_key,
				    full_key, sizeof(full_key)))
		return_NULL;
	mda = (struct metadata_area *) dm_hash_lookup(fid->metadata_areas_index,
						      full_key);

	return mda;
}

int fid_remove_mda(struct format_instance *fid, struct metadata_area *mda,
		   const char *key, size_t key_len, const unsigned sub_key)
{
	static char full_key[PATH_MAX];
	struct metadata_area *mda_indexed = NULL;

	/* At least one of mda or key must be specified. */
	if (!mda && !key)
		return 1;

	if (key) {
		/*
		 * If both mda and key specified, check given mda
		 * with what we find using the index and return
		 * immediately if these two do not match.
		 */
		if (!(mda_indexed = fid_get_mda_indexed(fid, key, key_len, sub_key)) ||
		     (mda && mda != mda_indexed))
			return 1;

		mda = mda_indexed;

		if (!_convert_key_to_string(key, key_len, sub_key,
				    full_key, sizeof(full_key)))
			return_0;

		dm_hash_remove(fid->metadata_areas_index, full_key);
	}

	dm_list_del(&mda->list);

	return 1;
}

/*
 * Copy constructor for a metadata_area.
 */
struct metadata_area *mda_copy(struct dm_pool *mem,
			       struct metadata_area *mda)
{
	struct metadata_area *mda_new;

	if (!(mda_new = dm_pool_alloc(mem, sizeof(*mda_new)))) {
		log_error("metadata_area allocation failed");
		return NULL;
	}
	memcpy(mda_new, mda, sizeof(*mda));
	if (mda->ops->mda_metadata_locn_copy && mda->metadata_locn) {
		mda_new->metadata_locn =
			mda->ops->mda_metadata_locn_copy(mem, mda->metadata_locn);
		if (!mda_new->metadata_locn) {
			dm_pool_free(mem, mda_new);
			return NULL;
		}
	}

	dm_list_init(&mda_new->list);

	return mda_new;
}
/*
 * This function provides a way to answer the question on a format specific
 * basis - does the format specfic context of these two metadata areas
 * match?
 *
 * A metatdata_area is defined to be independent of the underlying context.
 * This has the benefit that we can use the same abstraction to read disks
 * (see _metadata_text_raw_ops) or files (see _metadata_text_file_ops).
 * However, one downside is there is no format-independent way to determine
 * whether a given metadata_area is attached to a specific device - in fact,
 * it may not be attached to a device at all.
 *
 * Thus, LVM is structured such that an mda is not a member of struct
 * physical_volume.  The location of the mda depends on whether
 * the PV is in a volume group.  A PV not in a VG has an mda on the
 * 'info->mda' list in lvmcache, while a PV in a VG has an mda on
 * the vg->fid->metadata_areas_in_use list.  For further details, see _vg_read(),
 * and the sequence of creating the format_instance with fid->metadata_areas_in_use
 * list, as well as the construction of the VG, with list of PVs (comes
 * after the construction of the fid and list of mdas).
 */
unsigned mda_locns_match(struct metadata_area *mda1, struct metadata_area *mda2)
{
	if (!mda1->ops->mda_locns_match || !mda2->ops->mda_locns_match ||
	    mda1->ops->mda_locns_match != mda2->ops->mda_locns_match)
		return 0;

	return mda1->ops->mda_locns_match(mda1, mda2);
}

struct device *mda_get_device(struct metadata_area *mda)
{
	if (!mda->ops->mda_get_device)
		return NULL;
	return mda->ops->mda_get_device(mda);
}

unsigned mda_is_ignored(struct metadata_area *mda)
{
	return (mda->status & MDA_IGNORED);
}

void mda_set_ignored(struct metadata_area *mda, unsigned mda_ignored)
{
	void *locn = mda->metadata_locn;
	unsigned old_mda_ignored = mda_is_ignored(mda);

	if (mda_ignored && !old_mda_ignored)
		mda->status |= MDA_IGNORED;
	else if (!mda_ignored && old_mda_ignored)
		mda->status &= ~MDA_IGNORED;
	else
		return;	/* No change */

	log_debug("%s ignored flag for mda %s at offset %" PRIu64 ".", 
		  mda_ignored ? "Setting" : "Clearing",
		  mda->ops->mda_metadata_locn_name ? mda->ops->mda_metadata_locn_name(locn) : "",
		  mda->ops->mda_metadata_locn_offset ? mda->ops->mda_metadata_locn_offset(locn) : UINT64_C(0));
}

int mdas_empty_or_ignored(struct dm_list *mdas)
{
	struct metadata_area *mda;

	if (!dm_list_size(mdas))
		return 1;
	dm_list_iterate_items(mda, mdas) {
		if (mda_is_ignored(mda))
			return 1;
	}
	return 0;
}

int pv_change_metadataignore(struct physical_volume *pv, uint32_t mda_ignored)
{
	const char *pv_name = pv_dev_name(pv);

	if (mda_ignored && !pv_mda_used_count(pv)) {
		log_error("Metadata areas on physical volume \"%s\" already "
			  "ignored.", pv_name);
		return 0;
	}

	if (!mda_ignored && (pv_mda_used_count(pv) == pv_mda_count(pv))) {
		log_error("Metadata areas on physical volume \"%s\" already "
			  "marked as in-use.", pv_name);
		return 0;
	}

	if (!pv_mda_count(pv)) {
		log_error("Physical volume \"%s\" has no metadata "
			  "areas.", pv_name);
		return 0;
	}

	log_verbose("Marking metadata areas on physical volume \"%s\" "
		    "as %s.", pv_name, mda_ignored ? "ignored" : "in-use");

	if (!pv_mda_set_ignored(pv, mda_ignored))
		return_0;

	/*
	 * Update vg_mda_copies based on the mdas in this PV.
	 * This is most likely what the user would expect - if they
	 * specify a specific PV to be ignored/un-ignored, they will
	 * most likely not want LVM to turn around and change the
	 * ignore / un-ignore value when it writes the VG to disk.
	 * This does not guarantee this PV's ignore bits will be
	 * preserved in future operations.
	 */
	if (!is_orphan(pv) &&
	    vg_mda_copies(pv->vg) != VGMETADATACOPIES_UNMANAGED) {
		log_warn("WARNING: Changing preferred number of copies of VG %s "
			 "metadata from %"PRIu32" to %"PRIu32, pv_vg_name(pv),
			 vg_mda_copies(pv->vg), vg_mda_used_count(pv->vg));
		vg_set_mda_copies(pv->vg, vg_mda_used_count(pv->vg));
	}

	return 1;
}

char *tags_format_and_copy(struct dm_pool *mem, const struct dm_list *tags)
{
	struct str_list *sl;

	if (!dm_pool_begin_object(mem, 256)) {
		log_error("dm_pool_begin_object failed");
		return NULL;
	}

	dm_list_iterate_items(sl, tags) {
		if (!dm_pool_grow_object(mem, sl->str, strlen(sl->str)) ||
		    (sl->list.n != tags && !dm_pool_grow_object(mem, ",", 1))) {
			log_error("dm_pool_grow_object failed");
			return NULL;
		}
	}

	if (!dm_pool_grow_object(mem, "\0", 1)) {
		log_error("dm_pool_grow_object failed");
		return NULL;
	}
	return dm_pool_end_object(mem);
}

/**
 * pv_by_path - Given a device path return a PV handle if it is a PV
 * @cmd - handle to the LVM command instance
 * @pv_name - device path to read for the PV
 *
 * Returns:
 *  NULL - device path does not contain a valid PV
 *  non-NULL - PV handle corresponding to device path
 *
 * FIXME: merge with find_pv_by_name ?
 */
struct physical_volume *pv_by_path(struct cmd_context *cmd, const char *pv_name)
{
	return _pv_read(cmd, cmd->mem, pv_name, NULL, 1, 0);
}
