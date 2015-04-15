/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2010 Red Hat, Inc. All rights reserved.
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
#include "display.h"
#include "activate.h"
#include "toolcontext.h"
#include "lvmcache.h"
#include "archiver.h"

struct volume_group *alloc_vg(const char *pool_name, struct cmd_context *cmd,
			      const char *vg_name)
{
	struct dm_pool *vgmem;
	struct volume_group *vg;

	if (!(vgmem = dm_pool_create(pool_name, VG_MEMPOOL_CHUNK)) ||
	    !(vg = dm_pool_zalloc(vgmem, sizeof(*vg)))) {
		log_error("Failed to allocate volume group structure");
		if (vgmem)
			dm_pool_destroy(vgmem);
		return NULL;
	}

	if (vg_name && !(vg->name = dm_pool_strdup(vgmem, vg_name))) {
		log_error("Failed to allocate VG name.");
		dm_pool_destroy(vgmem);
		return NULL;
	}

	if (!(vg->lvm1_system_id = dm_pool_zalloc(vgmem, NAME_LEN + 1))) {
		log_error("Failed to allocate VG systemd id.");
		dm_pool_destroy(vgmem);
		return NULL;
	}

	vg->system_id = "";

	vg->cmd = cmd;
	vg->vgmem = vgmem;
	vg->alloc = ALLOC_NORMAL;

	if (!(vg->hostnames = dm_hash_create(16))) {
		log_error("Failed to allocate VG hostname hashtable.");
		dm_pool_destroy(vgmem);
		return NULL;
	}

	dm_list_init(&vg->pvs);
	dm_list_init(&vg->pvs_to_create);
	dm_list_init(&vg->pvs_outdated);
	dm_list_init(&vg->lvs);
	dm_list_init(&vg->tags);
	dm_list_init(&vg->removed_lvs);
	dm_list_init(&vg->removed_pvs);

	log_debug_mem("Allocated VG %s at %p.", vg->name, vg);

	return vg;
}

static void _free_vg(struct volume_group *vg)
{
	vg_set_fid(vg, NULL);

	if (vg->cmd && vg->vgmem == vg->cmd->mem) {
		log_error(INTERNAL_ERROR "global memory pool used for VG %s",
			  vg->name);
		return;
	}

	log_debug_mem("Freeing VG %s at %p.", vg->name, vg);

	dm_hash_destroy(vg->hostnames);
	dm_pool_destroy(vg->vgmem);
}

void release_vg(struct volume_group *vg)
{
	if (!vg || (vg->fid && vg == vg->fid->fmt->orphan_vg))
		return;

	/* Check if there are any vginfo holders */
	if (vg->vginfo &&
	    !lvmcache_vginfo_holders_dec_and_test_for_zero(vg->vginfo))
		return;

	release_vg(vg->vg_ondisk);
	release_vg(vg->vg_precommitted);
	if (vg->cft_precommitted)
		dm_config_destroy(vg->cft_precommitted);
	_free_vg(vg);
}

/*
 * FIXME out of place, but the main (cmd) pool has been already
 * destroyed and touching the fid (also via release_vg) will crash the
 * program
 *
 * For now quick wrapper to allow destroy of orphan vg
 */
void free_orphan_vg(struct volume_group *vg)
{
	_free_vg(vg);
}

char *vg_fmt_dup(const struct volume_group *vg)
{
	if (!vg->fid || !vg->fid->fmt)
		return NULL;
	return dm_pool_strdup(vg->vgmem, vg->fid->fmt->name);
}

char *vg_name_dup(const struct volume_group *vg)
{
	return dm_pool_strdup(vg->vgmem, vg->name);
}

char *vg_system_id_dup(const struct volume_group *vg)
{
	return dm_pool_strdup(vg->vgmem, vg->system_id ? : vg->lvm1_system_id ? : "");
}

char *vg_uuid_dup(const struct volume_group *vg)
{
	return id_format_and_copy(vg->vgmem, &vg->id);
}

char *vg_tags_dup(const struct volume_group *vg)
{
	return tags_format_and_copy(vg->vgmem, &vg->tags);
}

uint32_t vg_seqno(const struct volume_group *vg)
{
	return vg->seqno;
}

uint64_t vg_status(const struct volume_group *vg)
{
	return vg->status;
}

uint64_t vg_size(const struct volume_group *vg)
{
	return (uint64_t) vg->extent_count * vg->extent_size;
}

uint64_t vg_free(const struct volume_group *vg)
{
	return (uint64_t) vg->free_count * vg->extent_size;
}

uint64_t vg_extent_size(const struct volume_group *vg)
{
	return (uint64_t) vg->extent_size;
}

uint64_t vg_extent_count(const struct volume_group *vg)
{
	return (uint64_t) vg->extent_count;
}

uint64_t vg_free_count(const struct volume_group *vg)
{
	return (uint64_t) vg->free_count;
}

uint64_t vg_pv_count(const struct volume_group *vg)
{
	return (uint64_t) vg->pv_count;
}

uint64_t vg_max_pv(const struct volume_group *vg)
{
	return (uint64_t) vg->max_pv;
}

uint64_t vg_max_lv(const struct volume_group *vg)
{
	return (uint64_t) vg->max_lv;
}

unsigned snapshot_count(const struct volume_group *vg)
{
	struct lv_list *lvl;
	unsigned num_snapshots = 0;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_is_cow(lvl->lv))
			num_snapshots++;

	return num_snapshots;
}

unsigned vg_visible_lvs(const struct volume_group *vg)
{
	struct lv_list *lvl;
	unsigned lv_count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (lv_is_visible(lvl->lv))
			lv_count++;
	}

	return lv_count;
}

uint32_t vg_mda_count(const struct volume_group *vg)
{
	return dm_list_size(&vg->fid->metadata_areas_in_use) +
		dm_list_size(&vg->fid->metadata_areas_ignored);
}

uint32_t vg_mda_used_count(const struct volume_group *vg)
{
       uint32_t used_count = 0;
       struct metadata_area *mda;

	/*
	 * Ignored mdas could be on either list - the reason being the state
	 * may have changed from ignored to un-ignored and we need to write
	 * the state to disk.
	 */
       dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use)
	       if (!mda_is_ignored(mda))
		       used_count++;

       return used_count;
}

uint32_t vg_mda_copies(const struct volume_group *vg)
{
	return vg->mda_copies;
}

uint64_t vg_mda_size(const struct volume_group *vg)
{
	return find_min_mda_size(&vg->fid->metadata_areas_in_use);
}

uint64_t vg_mda_free(const struct volume_group *vg)
{
	uint64_t freespace = UINT64_MAX, mda_free;
	struct metadata_area *mda;

	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use) {
		if (!mda->ops->mda_free_sectors)
			continue;
		mda_free = mda->ops->mda_free_sectors(mda);
		if (mda_free < freespace)
			freespace = mda_free;
	}

	if (freespace == UINT64_MAX)
		freespace = UINT64_C(0);
	return freespace;
}

int vg_set_mda_copies(struct volume_group *vg, uint32_t mda_copies)
{
	vg->mda_copies = mda_copies;

	/* FIXME Use log_verbose when this is due to specific cmdline request. */
	log_debug_metadata("Setting mda_copies to %"PRIu32" for VG %s",
			   mda_copies, vg->name);

	return 1;
}

char *vg_profile_dup(const struct volume_group *vg)
{
	const char *profile_name = vg->profile ? vg->profile->name : "";
	return dm_pool_strdup(vg->vgmem, profile_name);
}

static int _recalc_extents(uint32_t *extents, const char *desc1,
			   const char *desc2, uint32_t old_extent_size,
			   uint32_t new_extent_size)
{
	uint64_t size = (uint64_t) old_extent_size * (*extents);

	if (size % new_extent_size) {
		log_error("New size %" PRIu64 " for %s%s not an exact number "
			  "of new extents.", size, desc1, desc2);
		return 0;
	}

	size /= new_extent_size;

	if (size > MAX_EXTENT_COUNT) {
		log_error("New extent count %" PRIu64 " for %s%s exceeds "
			  "32 bits.", size, desc1, desc2);
		return 0;
	}

	*extents = (uint32_t) size;

	return 1;
}

int vg_check_new_extent_size(const struct format_type *fmt, uint32_t new_extent_size)
{
	if (!new_extent_size) {
		log_error("Physical extent size may not be zero");
		return 0;
	}

	if ((fmt->features & FMT_NON_POWER2_EXTENTS)) {
		if ((new_extent_size & (new_extent_size - 1)) &&
		    (new_extent_size % MIN_NON_POWER2_EXTENT_SIZE)) {
			log_error("Physical Extent size must be a multiple of %s when not a power of 2.",
				  display_size(fmt->cmd, (uint64_t) MIN_NON_POWER2_EXTENT_SIZE));
			return 0;
		}
		return 1;
	}

	/* Apply original format1 restrictions */
	if ((new_extent_size & (new_extent_size - 1))) {
		log_error("Metadata format only supports Physical Extent sizes that are powers of 2.");
		return 0;
	}

	if (new_extent_size > MAX_PE_SIZE || new_extent_size < MIN_PE_SIZE) {
		log_error("Extent size must be between %s and %s",
			  display_size(fmt->cmd, (uint64_t) MIN_PE_SIZE),
			  display_size(fmt->cmd, (uint64_t) MAX_PE_SIZE));
		return 0;
	}

	if (new_extent_size % MIN_PE_SIZE) {
		log_error("Extent size must be multiple of %s",
			  display_size(fmt->cmd, (uint64_t) MIN_PE_SIZE));
		return 0;
	}

 	return 1;
}

int vg_set_extent_size(struct volume_group *vg, uint32_t new_extent_size)
{
	uint32_t old_extent_size = vg->extent_size;
	struct pv_list *pvl;
	struct lv_list *lvl;
	struct physical_volume *pv;
	struct logical_volume *lv;
	struct lv_segment *seg;
	struct pv_segment *pvseg;
	uint32_t s;

	if (!vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" must be resizeable "
			  "to change PE size", vg->name);
		return 0;
	}

	if (new_extent_size == vg->extent_size)
		return 1;

	if (!vg_check_new_extent_size(vg->fid->fmt, new_extent_size))
		return_0;

	if (new_extent_size > vg->extent_size) {
		if ((uint64_t) vg_size(vg) % new_extent_size) {
			/* FIXME Adjust used PV sizes instead */
			log_error("New extent size is not a perfect fit");
			return 0;
		}
	}

	vg->extent_size = new_extent_size;

	if (vg->fid->fmt->ops->vg_setup &&
	    !vg->fid->fmt->ops->vg_setup(vg->fid, vg))
		return_0;

	if (!_recalc_extents(&vg->extent_count, vg->name, "", old_extent_size,
			     new_extent_size))
		return_0;

	if (!_recalc_extents(&vg->free_count, vg->name, " free space",
			     old_extent_size, new_extent_size))
		return_0;

	/* foreach PV */
	dm_list_iterate_items(pvl, &vg->pvs) {
		pv = pvl->pv;

		pv->pe_size = new_extent_size;
		if (!_recalc_extents(&pv->pe_count, pv_dev_name(pv), "",
				     old_extent_size, new_extent_size))
			return_0;

		if (!_recalc_extents(&pv->pe_alloc_count, pv_dev_name(pv),
				     " allocated space", old_extent_size, new_extent_size))
			return_0;

		/* foreach free PV Segment */
		dm_list_iterate_items(pvseg, &pv->segments) {
			if (pvseg_is_allocated(pvseg))
				continue;

			if (!_recalc_extents(&pvseg->pe, pv_dev_name(pv),
					     " PV segment start", old_extent_size,
					     new_extent_size))
				return_0;
			if (!_recalc_extents(&pvseg->len, pv_dev_name(pv),
					     " PV segment length", old_extent_size,
					     new_extent_size))
				return_0;
		}
	}

	/* foreach LV */
	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!_recalc_extents(&lv->le_count, lv->name, "", old_extent_size,
				     new_extent_size))
			return_0;

		dm_list_iterate_items(seg, &lv->segments) {
			if (!_recalc_extents(&seg->le, lv->name,
					     " segment start", old_extent_size,
					     new_extent_size))
				return_0;

			if (!_recalc_extents(&seg->len, lv->name,
					     " segment length", old_extent_size,
					     new_extent_size))
				return_0;

			if (!_recalc_extents(&seg->area_len, lv->name,
					     " area length", old_extent_size,
					     new_extent_size))
				return_0;

			if (!_recalc_extents(&seg->extents_copied, lv->name,
					     " extents moved", old_extent_size,
					     new_extent_size))
				return_0;

			/* foreach area */
			for (s = 0; s < seg->area_count; s++) {
				switch (seg_type(seg, s)) {
				case AREA_PV:
					if (!_recalc_extents
					    (&seg_pe(seg, s),
					     lv->name,
					     " pvseg start", old_extent_size,
					     new_extent_size))
						return_0;
					if (!_recalc_extents
					    (&seg_pvseg(seg, s)->len,
					     lv->name,
					     " pvseg length", old_extent_size,
					     new_extent_size))
						return_0;
					break;
				case AREA_LV:
					if (!_recalc_extents
					    (&seg_le(seg, s), lv->name,
					     " area start", old_extent_size,
					     new_extent_size))
						return_0;
					break;
				case AREA_UNASSIGNED:
					log_error("Unassigned area %u found in "
						  "segment", s);
					return 0;
				}
			}
		}

	}

	return 1;
}

int vg_set_max_lv(struct volume_group *vg, uint32_t max_lv)
{
	if (!vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" must be resizeable "
			  "to change MaxLogicalVolume", vg->name);
		return 0;
	}

	if (!(vg->fid->fmt->features & FMT_UNLIMITED_VOLS)) {
		if (!max_lv)
			max_lv = 255;
		else if (max_lv > 255) {
			log_error("MaxLogicalVolume limit is 255");
			return 0;
		}
	}

	if (max_lv && max_lv < vg_visible_lvs(vg)) {
		log_error("MaxLogicalVolume is less than the current number "
			  "%d of LVs for %s", vg_visible_lvs(vg),
			  vg->name);
		return 0;
	}
	vg->max_lv = max_lv;

	return 1;
}

int vg_set_max_pv(struct volume_group *vg, uint32_t max_pv)
{
	if (!vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" must be resizeable "
			  "to change MaxPhysicalVolumes", vg->name);
		return 0;
	}

	if (!(vg->fid->fmt->features & FMT_UNLIMITED_VOLS)) {
		if (!max_pv)
			max_pv = 255;
		else if (max_pv > 255) {
			log_error("MaxPhysicalVolume limit is 255");
			return 0;
		}
	}

	if (max_pv && max_pv < vg->pv_count) {
		log_error("MaxPhysicalVolumes is less than the current number "
			  "%d of PVs for \"%s\"", vg->pv_count,
			  vg->name);
		return 0;
	}
	vg->max_pv = max_pv;
	return 1;
}

int vg_set_alloc_policy(struct volume_group *vg, alloc_policy_t alloc)
{
	if (alloc == ALLOC_INHERIT) {
		log_error("Volume Group allocation policy cannot inherit "
			  "from anything");
		return 0;
	}

	if (alloc == vg->alloc)
		return 1;

	vg->alloc = alloc;
	return 1;
}

/*
 * Setting the cluster attribute marks active volumes exclusive.
 *
 * FIXME: resolve logic with reacquiring proper top-level LV locks
 *        and we likely can't giveup DLM locks for active LVs...
 */
int vg_set_clustered(struct volume_group *vg, int clustered)
{
	struct lv_list *lvl;
	int fail = 0;

	if (vg_is_clustered(vg) &&
	    locking_is_clustered() &&
	    locking_supports_remote_queries() &&
	    !clustered) {
		/*
		 * If the volume is locally active but not exclusively
		 * we cannot determine when other nodes also use
		 * locally active (CR lock), so refuse conversion.
		 */
		dm_list_iterate_items(lvl, &vg->lvs)
			if ((lv_lock_holder(lvl->lv) == lvl->lv) &&
			    lv_is_active(lvl->lv) &&
			    !lv_is_active_exclusive_locally(lvl->lv)) {
				/* Show all non-local-exclusively active LVs
				 * this includes i.e. clustered mirrors */
				log_error("Can't change cluster attribute with "
					  "active logical volume %s.",
					  display_lvname(lvl->lv));
				fail = 1;
			}

		if (fail) {
			log_print_unless_silent("Conversion is supported only for "
						"locally exclusive volumes.");
			return 0;
		}
	}

	if (clustered)
		vg->status |= CLUSTERED;
	else
		vg->status &= ~CLUSTERED;

	log_debug_metadata("Setting volume group %s as %sclustered.",
			   vg->name, clustered ? "" : "not " );

	return 1;
}

/* The input string has already been validated. */

int vg_set_system_id(struct volume_group *vg, const char *system_id)
{
	if (!system_id || !*system_id) {
		vg->system_id = NULL;
		return 1;
	}

	if (systemid_on_pvs(vg)) {
		log_error("Metadata format %s does not support this type of system ID.",
			  vg->fid->fmt->name);
		return 0;
	}

	if (!(vg->system_id = dm_pool_strdup(vg->vgmem, system_id))) {
		log_error("Failed to allocate memory for system_id in vg_set_system_id.");
		return 0;
	}

	if (vg->lvm1_system_id)
		*vg->lvm1_system_id = '\0';

	return 1;
}

char *vg_attr_dup(struct dm_pool *mem, const struct volume_group *vg)
{
	char *repstr;

	if (!(repstr = dm_pool_zalloc(mem, 7))) {
		log_error("dm_pool_alloc failed");
		return NULL;
	}

	repstr[0] = (vg->status & LVM_WRITE) ? 'w' : 'r';
	repstr[1] = (vg_is_resizeable(vg)) ? 'z' : '-';
	repstr[2] = (vg_is_exported(vg)) ? 'x' : '-';
	repstr[3] = (vg_missing_pv_count(vg)) ? 'p' : '-';
	repstr[4] = alloc_policy_char(vg->alloc);
	repstr[5] = (vg_is_clustered(vg)) ? 'c' : '-';
	return repstr;
}

int vgreduce_single(struct cmd_context *cmd, struct volume_group *vg,
			    struct physical_volume *pv, int commit)
{
	struct pv_list *pvl;
	struct volume_group *orphan_vg = NULL;
	int r = 0;
	const char *name = pv_dev_name(pv);

	if (!vg) {
		log_error(INTERNAL_ERROR "VG is NULL.");
		return r;
	}

	if (pv_pe_alloc_count(pv)) {
		log_error("Physical volume \"%s\" still in use", name);
		return r;
	}

	if (vg->pv_count == 1) {
		log_error("Can't remove final physical volume \"%s\" from "
			  "volume group \"%s\"", name, vg->name);
		return r;
	}

	if (!lock_vol(cmd, VG_ORPHANS, LCK_VG_WRITE, NULL)) {
		log_error("Can't get lock for orphan PVs");
		return r;
	}

	pvl = find_pv_in_vg(vg, name);

	if (!archive(vg))
		goto_bad;

	log_verbose("Removing \"%s\" from volume group \"%s\"", name, vg->name);

	if (pvl)
		del_pvl_from_vgs(vg, pvl);

	pv->vg_name = vg->fid->fmt->orphan_vg_name;
	pv->status = ALLOCATABLE_PV;

	if (!dev_get_size(pv_dev(pv), &pv->size)) {
		log_error("%s: Couldn't get size.", pv_dev_name(pv));
		goto bad;
	}

	vg->free_count -= pv_pe_count(pv) - pv_pe_alloc_count(pv);
	vg->extent_count -= pv_pe_count(pv);

	orphan_vg = vg_read_for_update(cmd, vg->fid->fmt->orphan_vg_name,
				       NULL, 0);

	if (vg_read_error(orphan_vg))
		goto bad;

	if (!vg_split_mdas(cmd, vg, orphan_vg) || !vg->pv_count) {
		log_error("Cannot remove final metadata area on \"%s\" from \"%s\"",
			  name, vg->name);
		goto bad;
	}

	/*
	 * Only write out the needed changes if so requested by caller.
	 */
	if (commit) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Removal of physical volume \"%s\" from "
				  "\"%s\" failed", name, vg->name);
			goto bad;
		}

		if (!pv_write(cmd, pv, 0)) {
			log_error("Failed to clear metadata from physical "
				  "volume \"%s\" "
				  "after removal from \"%s\"", name, vg->name);
			goto bad;
		}

		backup(vg);

		log_print_unless_silent("Removed \"%s\" from volume group \"%s\"",
				name, vg->name);
	}
	r = 1;
bad:
	/* If we are committing here or we had an error then we will free fid */
	if (pvl && (commit || r != 1))
		free_pv_fid(pvl->pv);
	unlock_and_release_vg(cmd, orphan_vg, VG_ORPHANS);
	return r;
}
