/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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
#include "toolcontext.h"
#include "segtype.h"
#include "display.h"
#include "archiver.h"
#include "activate.h"
#include "lv_alloc.h"
#include "lvm-string.h"
#include "str_list.h"
#include "memlock.h"

#define RAID_REGION_SIZE 1024

static int _lv_is_raid_with_tracking(const struct logical_volume *lv,
				     struct logical_volume **tracking)
{
	uint32_t s;
	struct lv_segment *seg;

	*tracking = NULL;
	seg = first_seg(lv);

	if (!(lv->status & RAID))
		return 0;

	for (s = 0; s < seg->area_count; s++)
		if (lv_is_visible(seg_lv(seg, s)) &&
		    !(seg_lv(seg, s)->status & LVM_WRITE))
			*tracking = seg_lv(seg, s);


	return *tracking ? 1 : 0;
}

int lv_is_raid_with_tracking(const struct logical_volume *lv)
{
	struct logical_volume *tracking;

	return _lv_is_raid_with_tracking(lv, &tracking);
}

uint32_t lv_raid_image_count(const struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_raid(seg))
		return 1;

	return seg->area_count;
}

static int _activate_sublv_preserving_excl(struct logical_volume *top_lv,
					   struct logical_volume *sub_lv)
{
	struct cmd_context *cmd = top_lv->vg->cmd;

	/* If top RAID was EX, use EX */
	if (lv_is_active_exclusive_locally(top_lv)) {
		if (!activate_lv_excl(cmd, sub_lv))
			return_0;
	} else {
		if (!activate_lv(cmd, sub_lv))
			return_0;
	}
	return 1;
}

/*
 * _lv_is_on_pv
 * @lv:
 * @pv:
 *
 * If any of the component devices of the LV are on the given PV, 1
 * is returned; otherwise 0.  For example if one of the images of a RAID
 * (or its metadata device) is on the PV, 1 would be returned for the
 * top-level LV.
 * If you wish to check the images themselves, you should pass them.
 *
 * FIXME:  This should be made more generic, possibly use 'for_each_sub_lv',
 * and be put in lv_manip.c.  'for_each_sub_lv' does not yet allow us to
 * short-circuit execution or pass back the values we need yet though...
 */
static int _lv_is_on_pv(struct logical_volume *lv, struct physical_volume *pv)
{
	uint32_t s;
	struct physical_volume *pv2;
	struct lv_segment *seg;

	if (!lv)
		return 0;

	seg = first_seg(lv);
	if (!seg)
		return 0;

	/* Check mirror log */
	if (_lv_is_on_pv(seg->log_lv, pv))
		return 1;

	/* Check stack of LVs */
	dm_list_iterate_items(seg, &lv->segments) {
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) == AREA_PV) {
				pv2 = seg_pv(seg, s);
				if (id_equal(&pv->id, &pv2->id))
					return 1;
				if (pv->dev && pv2->dev &&
				    (pv->dev->dev == pv2->dev->dev))
					return 1;
			}

			if ((seg_type(seg, s) == AREA_LV) &&
			    _lv_is_on_pv(seg_lv(seg, s), pv))
				return 1;

			if (!seg_is_raid(seg))
				continue;

			/* This is RAID, so we know the meta_area is AREA_LV */
			if (_lv_is_on_pv(seg_metalv(seg, s), pv))
				return 1;
		}
	}

	return 0;
}

static int _lv_is_on_pvs(struct logical_volume *lv, struct dm_list *pvs)
{
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, pvs)
		if (_lv_is_on_pv(lv, pvl->pv)) {
			log_debug("%s is on %s", lv->name,
				  pv_dev_name(pvl->pv));
			return 1;
		} else
			log_debug("%s is not on %s", lv->name,
				  pv_dev_name(pvl->pv));
	return 0;
}

static int _get_pv_list_for_lv(struct logical_volume *lv, struct dm_list *pvs)
{
	uint32_t s;
	struct pv_list *pvl;
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_linear(seg)) {
		log_error(INTERNAL_ERROR
			  "_get_pv_list_for_lv only handles linear volumes");
		return 0;
	}

	log_debug("Getting list of PVs that %s/%s is on:",
		  lv->vg->name, lv->name);

	dm_list_iterate_items(seg, &lv->segments) {
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) != AREA_PV) {
				log_error(INTERNAL_ERROR
					  "Linear seg_type should be AREA_PV");
				return 0;
			}

			if (!(pvl = dm_pool_zalloc(lv->vg->cmd->mem,
						   sizeof(*pvl)))) {
				log_error("Failed to allocate memory");
				return 0;
			}

			pvl->pv = seg_pv(seg, s);
			log_debug("  %s/%s is on %s", lv->vg->name, lv->name,
				  pv_dev_name(pvl->pv));
			dm_list_add(pvs, &pvl->list);
		}
	}

	return 1;
}

/*
 * _raid_in_sync
 * @lv
 *
 * _raid_in_sync works for all types of RAID segtypes, as well
 * as 'mirror' segtype.  (This is because 'lv_raid_percent' is
 * simply a wrapper around 'lv_mirror_percent'.
 *
 * Returns: 1 if in-sync, 0 otherwise.
 */
static int _raid_in_sync(struct logical_volume *lv)
{
	percent_t sync_percent;

	if (!lv_raid_percent(lv, &sync_percent)) {
		log_error("Unable to determine sync status of %s/%s.",
			  lv->vg->name, lv->name);
		return 0;
	}

	return (sync_percent == PERCENT_100) ? 1 : 0;
}

/*
 * _raid_remove_top_layer
 * @lv
 * @removal_list
 *
 * Remove top layer of RAID LV in order to convert to linear.
 * This function makes no on-disk changes.  The residual LVs
 * returned in 'removal_list' must be freed by the caller.
 *
 * Returns: 1 on succes, 0 on failure
 */
static int _raid_remove_top_layer(struct logical_volume *lv,
				  struct dm_list *removal_list)
{
	struct lv_list *lvl_array, *lvl;
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_mirrored(seg)) {
		log_error(INTERNAL_ERROR
			  "Unable to remove RAID layer from segment type %s",
			  seg->segtype->name);
		return 0;
	}

	if (seg->area_count != 1) {
		log_error(INTERNAL_ERROR
			  "Unable to remove RAID layer when there"
			  " is more than one sub-lv");
		return 0;
	}

	lvl_array = dm_pool_alloc(lv->vg->vgmem, 2 * sizeof(*lvl));
	if (!lvl_array) {
		log_error("Memory allocation failed.");
		return 0;
	}

	/* Add last metadata area to removal_list */
	lvl_array[0].lv = seg_metalv(seg, 0);
	lv_set_visible(seg_metalv(seg, 0));
	remove_seg_from_segs_using_this_lv(seg_metalv(seg, 0), seg);
	seg_metatype(seg, 0) = AREA_UNASSIGNED;
	dm_list_add(removal_list, &(lvl_array[0].list));

	/* Remove RAID layer and add residual LV to removal_list*/
	seg_lv(seg, 0)->status &= ~RAID_IMAGE;
	lv_set_visible(seg_lv(seg, 0));
	lvl_array[1].lv = seg_lv(seg, 0);
	dm_list_add(removal_list, &(lvl_array[1].list));

	if (!remove_layer_from_lv(lv, seg_lv(seg, 0)))
		return_0;

	lv->status &= ~(MIRRORED | RAID);
	return 1;
}

/*
 * _clear_lv
 * @lv
 *
 * If LV is active:
 *        clear first block of device
 * otherwise:
 *        activate, clear, deactivate
 *
 * Returns: 1 on success, 0 on failure
 */
static int _clear_lv(struct logical_volume *lv)
{
	int was_active = lv_is_active(lv);

	if (!was_active && !activate_lv(lv->vg->cmd, lv)) {
		log_error("Failed to activate %s for clearing",
			  lv->name);
		return 0;
	}

	log_verbose("Clearing metadata area of %s/%s",
		    lv->vg->name, lv->name);
	/*
	 * Rather than wiping lv->size, we can simply
	 * wipe the first sector to remove the superblock of any previous
	 * RAID devices.  It is much quicker.
	 */
	if (!set_lv(lv->vg->cmd, lv, 1, 0)) {
		log_error("Failed to zero %s", lv->name);
		return 0;
	}

	if (!was_active && !deactivate_lv(lv->vg->cmd, lv)) {
		log_error("Failed to deactivate %s", lv->name);
		return 0;
	}

	return 1;
}

/* Makes on-disk metadata changes */
static int _clear_lvs(struct dm_list *lv_list)
{
	struct lv_list *lvl;
	struct volume_group *vg = NULL;

	if (dm_list_empty(lv_list)) {
		log_debug(INTERNAL_ERROR "Empty list of LVs given for clearing");
		return 1;
	}

	dm_list_iterate_items(lvl, lv_list) {
		if (!lv_is_visible(lvl->lv)) {
			log_error(INTERNAL_ERROR
				  "LVs must be set visible before clearing");
			return 0;
		}
		vg = lvl->lv->vg;
	}

	/*
	 * FIXME: only vg_[write|commit] if LVs are not already written
	 * as visible in the LVM metadata (which is never the case yet).
	 */
	if (!vg || !vg_write(vg) || !vg_commit(vg))
		return_0;

	dm_list_iterate_items(lvl, lv_list)
		if (!_clear_lv(lvl->lv))
			return 0;

	return 1;
}

/*
 * _shift_and_rename_image_components
 * @seg: Top-level RAID segment
 *
 * Shift all higher indexed segment areas down to fill in gaps where
 * there are 'AREA_UNASSIGNED' areas and rename data/metadata LVs so
 * that their names match their new index.  When finished, set
 * seg->area_count to new reduced total.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _shift_and_rename_image_components(struct lv_segment *seg)
{
	int len;
	char *shift_name;
	uint32_t s, missing;
	struct cmd_context *cmd = seg->lv->vg->cmd;

	/*
	 * All LVs must be properly named for their index before
	 * shifting begins.  (e.g.  Index '0' must contain *_rimage_0 and
	 * *_rmeta_0.  Index 'n' must contain *_rimage_n and *_rmeta_n.)
	 */

	if (!seg_is_raid(seg))
		return_0;

	if (seg->area_count > 10) {
		/*
		 * FIXME: Handling more would mean I'd have
		 * to handle double digits
		 */
		log_error("Unable handle arrays with more than 10 devices");
		return 0;
	}

	log_very_verbose("Shifting images in %s", seg->lv->name);

	for (s = 0, missing = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) == AREA_UNASSIGNED) {
			if (seg_metatype(seg, s) != AREA_UNASSIGNED) {
				log_error(INTERNAL_ERROR "Metadata segment area"
					  " #%d should be AREA_UNASSIGNED", s);
				return 0;
			}
			missing++;
			continue;
		}
		if (!missing)
			continue;

		log_very_verbose("Shifting %s and %s by %u",
				 seg_metalv(seg, s)->name,
				 seg_lv(seg, s)->name, missing);

		/* Alter rmeta name */
		shift_name = dm_pool_strdup(cmd->mem, seg_metalv(seg, s)->name);
		if (!shift_name) {
			log_error("Memory allocation failed.");
			return 0;
		}
		len = strlen(shift_name) - 1;
		shift_name[len] -= missing;
		seg_metalv(seg, s)->name = shift_name;

		/* Alter rimage name */
		shift_name = dm_pool_strdup(cmd->mem, seg_lv(seg, s)->name);
		if (!shift_name) {
			log_error("Memory allocation failed.");
			return 0;
		}
		len = strlen(shift_name) - 1;
		shift_name[len] -= missing;
		seg_lv(seg, s)->name = shift_name;

		seg->areas[s - missing] = seg->areas[s];
		seg->meta_areas[s - missing] = seg->meta_areas[s];
	}

	seg->area_count -= missing;
	return 1;
}

/*
 * Create an LV of specified type.  Set visible after creation.
 * This function does not make metadata changes.
 */
static int _alloc_image_component(struct logical_volume *lv,
				  const char *alt_base_name,
				  struct alloc_handle *ah, uint32_t first_area,
				  uint64_t type, struct logical_volume **new_lv)
{
	uint64_t status;
	size_t len = strlen(lv->name) + 32;
	char img_name[len];
	const char *base_name = (alt_base_name) ? alt_base_name : lv->name;
	struct logical_volume *tmp_lv;
	const struct segment_type *segtype;

	if (type == RAID_META) {
		if (dm_snprintf(img_name, len, "%s_rmeta_%%d", base_name) < 0)
			return_0;
	} else if (type == RAID_IMAGE) {
		if (dm_snprintf(img_name, len, "%s_rimage_%%d", base_name) < 0)
			return_0;
	} else {
		log_error(INTERNAL_ERROR
			  "Bad type provided to _alloc_raid_component");
		return 0;
	}

	if (!ah) {
		first_area = 0;
		log_error(INTERNAL_ERROR
			  "Stand-alone %s area allocation not implemented",
			  (type == RAID_META) ? "metadata" : "data");
		return 0;
	}

	status = LVM_READ | LVM_WRITE | LV_REBUILD | type;
	tmp_lv = lv_create_empty(img_name, NULL, status, ALLOC_INHERIT, lv->vg);
	if (!tmp_lv) {
		log_error("Failed to allocate new raid component, %s", img_name);
		return 0;
	}

	segtype = get_segtype_from_string(lv->vg->cmd, "striped");
	if (!lv_add_segment(ah, first_area, 1, tmp_lv, segtype, 0, status, 0)) {
		log_error("Failed to add segment to LV, %s", img_name);
		return 0;
	}

	lv_set_visible(tmp_lv);
	*new_lv = tmp_lv;
	return 1;
}

static int _alloc_image_components(struct logical_volume *lv,
				   struct dm_list *pvs, uint32_t count,
				   struct dm_list *new_meta_lvs,
				   struct dm_list *new_data_lvs)
{
	uint32_t s;
	uint32_t region_size;
	struct lv_segment *seg = first_seg(lv);
	const struct segment_type *segtype;
	struct alloc_handle *ah;
	struct dm_list *parallel_areas;
	struct logical_volume *tmp_lv;
	struct lv_list *lvl_array;

	lvl_array = dm_pool_alloc(lv->vg->vgmem,
				  sizeof(*lvl_array) * count * 2);
	if (!lvl_array)
		return_0;

	if (!(parallel_areas = build_parallel_areas_from_lv(lv, 0)))
		return_0;

	if (seg_is_linear(seg))
		region_size = RAID_REGION_SIZE;
	else
		region_size = seg->region_size;

	if (seg_is_raid(seg))
		segtype = seg->segtype;
	else if (!(segtype = get_segtype_from_string(lv->vg->cmd, "raid1")))
		return_0;

	if (!(ah = allocate_extents(lv->vg, NULL, segtype, 0, count, count,
				    region_size, lv->le_count, pvs,
				    lv->alloc, parallel_areas)))
		return_0;

	for (s = 0; s < count; s++) {
		/*
		 * The allocation areas are grouped together.  First
		 * come the rimage allocated areas, then come the metadata
		 * allocated areas.  Thus, the metadata areas are pulled
		 * from 's + count'.
		 */
		if (!_alloc_image_component(lv, NULL, ah, s + count,
					    RAID_META, &tmp_lv))
			return_0;
		lvl_array[s + count].lv = tmp_lv;
		dm_list_add(new_meta_lvs, &(lvl_array[s + count].list));

		if (!_alloc_image_component(lv, NULL, ah, s,
					    RAID_IMAGE, &tmp_lv))
			return_0;
		lvl_array[s].lv = tmp_lv;
		dm_list_add(new_data_lvs, &(lvl_array[s].list));
	}
	alloc_destroy(ah);
	return 1;
}

/*
 * _alloc_rmeta_for_lv
 * @lv
 *
 * Allocate a RAID metadata device for the given LV (which is or will
 * be the associated RAID data device).  The new metadata device must
 * be allocated from the same PV(s) as the data device.
 */
static int _alloc_rmeta_for_lv(struct logical_volume *data_lv,
			       struct logical_volume **meta_lv)
{
	struct dm_list allocatable_pvs;
	struct alloc_handle *ah;
	struct lv_segment *seg = first_seg(data_lv);
	char *p, base_name[strlen(data_lv->name) + 1];

	dm_list_init(&allocatable_pvs);

	if (!seg_is_linear(seg)) {
		log_error(INTERNAL_ERROR "Unable to allocate RAID metadata "
			  "area for non-linear LV, %s", data_lv->name);
		return 0;
	}

	sprintf(base_name, "%s", data_lv->name);
	if ((p = strstr(base_name, "_mimage_")))
		*p = '\0';

	if (!_get_pv_list_for_lv(data_lv, &allocatable_pvs)) {
		log_error("Failed to build list of PVs for %s/%s",
			  data_lv->vg->name, data_lv->name);
		return 0;
	}

	if (!(ah = allocate_extents(data_lv->vg, NULL, seg->segtype, 0, 1, 0,
				    seg->region_size,
				    1 /*RAID_METADATA_AREA_LEN*/,
				    &allocatable_pvs, data_lv->alloc, NULL)))
		return_0;

	if (!_alloc_image_component(data_lv, base_name, ah, 0,
				    RAID_META, meta_lv))
		return_0;

	alloc_destroy(ah);
	return 1;
}

static int _raid_add_images(struct logical_volume *lv,
			    uint32_t new_count, struct dm_list *pvs)
{
	int rebuild_flag_cleared = 0;
	uint32_t s;
	uint32_t old_count = lv_raid_image_count(lv);
	uint32_t count = new_count - old_count;
	uint64_t status_mask = -1;
	struct cmd_context *cmd = lv->vg->cmd;
	struct lv_segment *seg = first_seg(lv);
	struct dm_list meta_lvs, data_lvs;
	struct lv_list *lvl;
	struct lv_segment_area *new_areas;

	dm_list_init(&meta_lvs); /* For image addition */
	dm_list_init(&data_lvs); /* For image addition */

	/*
	 * If the segtype is linear, then we must allocate a metadata
	 * LV to accompany it.
	 */
	if (seg_is_linear(seg)) {
		/* A complete resync will be done, no need to mark each sub-lv */
		status_mask = ~(LV_REBUILD);

		if (!(lvl = dm_pool_alloc(lv->vg->vgmem, sizeof(*lvl)))) {
			log_error("Memory allocation failed");
			return 0;
		}

		if (!_alloc_rmeta_for_lv(lv, &lvl->lv))
			return_0;

		dm_list_add(&meta_lvs, &lvl->list);
	} else if (!seg_is_raid(seg)) {
		log_error("Unable to add RAID images to %s of segment type %s",
			  lv->name, seg->segtype->name);
		return 0;
	}

	if (!_alloc_image_components(lv, pvs, count, &meta_lvs, &data_lvs)) {
		log_error("Failed to allocate new image components");
		return 0;
	}

	/*
	 * If linear, we must correct data LV names.  They are off-by-one
	 * because the linear volume hasn't taken its proper name of "_rimage_0"
	 * yet.  This action must be done before '_clear_lvs' because it
	 * commits the LVM metadata before clearing the LVs.
	 */
	if (seg_is_linear(seg)) {
		char *name;
		size_t len;
		struct dm_list *l;
		struct lv_list *lvl_tmp;

		dm_list_iterate(l, &data_lvs) {
			if (l == dm_list_last(&data_lvs)) {
				lvl = dm_list_item(l, struct lv_list);
				len = strlen(lv->name) + strlen("_rimage_XXX");
				name = dm_pool_alloc(lv->vg->vgmem, len);
				sprintf(name, "%s_rimage_%u", lv->name, count);
				lvl->lv->name = name;
				continue;
			}
			lvl = dm_list_item(l, struct lv_list);
			lvl_tmp = dm_list_item(l->n, struct lv_list);
			lvl->lv->name = lvl_tmp->lv->name;
		}
	}

	/* Metadata LVs must be cleared before being added to the array */
	if (!_clear_lvs(&meta_lvs))
		goto fail;

	if (seg_is_linear(seg)) {
		first_seg(lv)->status |= RAID_IMAGE;
		if (!insert_layer_for_lv(lv->vg->cmd, lv,
					 RAID | LVM_READ | LVM_WRITE,
					 "_rimage_0"))
			return_0;

		lv->status |= RAID;
		seg = first_seg(lv);
		seg_lv(seg, 0)->status |= RAID_IMAGE | LVM_READ | LVM_WRITE;
		seg->region_size = RAID_REGION_SIZE;
		seg->segtype = get_segtype_from_string(lv->vg->cmd, "raid1");
		if (!seg->segtype)
			return_0;
	}
/*
FIXME: It would be proper to activate the new LVs here, instead of having
them activated by the suspend.  However, this causes residual device nodes
to be left for these sub-lvs.
	dm_list_iterate_items(lvl, &meta_lvs)
		if (!do_correct_activate(lv, lvl->lv))
			return_0;
	dm_list_iterate_items(lvl, &data_lvs)
		if (!do_correct_activate(lv, lvl->lv))
			return_0;
*/
	/* Expand areas array */
	if (!(new_areas = dm_pool_zalloc(lv->vg->cmd->mem,
					 new_count * sizeof(*new_areas))))
		goto fail;
	memcpy(new_areas, seg->areas, seg->area_count * sizeof(*seg->areas));
	seg->areas = new_areas;

	/* Expand meta_areas array */
	if (!(new_areas = dm_pool_zalloc(lv->vg->cmd->mem,
					 new_count * sizeof(*new_areas))))
		goto fail;
	if (seg->meta_areas)
		memcpy(new_areas, seg->meta_areas,
		       seg->area_count * sizeof(*seg->meta_areas));
	seg->meta_areas = new_areas;
	seg->area_count = new_count;

	/* Add extra meta area when converting from linear */
	s = (old_count == 1) ? 0 : old_count;

	/* Set segment areas for metadata sub_lvs */
	dm_list_iterate_items(lvl, &meta_lvs) {
		log_debug("Adding %s to %s",
			  lvl->lv->name, lv->name);
		lvl->lv->status &= status_mask;
		first_seg(lvl->lv)->status &= status_mask;
		if (!set_lv_segment_area_lv(seg, s, lvl->lv, 0,
					    lvl->lv->status)) {
			log_error("Failed to add %s to %s",
				  lvl->lv->name, lv->name);
			goto fail;
		}
		s++;
	}

	s = old_count;

	/* Set segment areas for data sub_lvs */
	dm_list_iterate_items(lvl, &data_lvs) {
		log_debug("Adding %s to %s",
			  lvl->lv->name, lv->name);
		lvl->lv->status &= status_mask;
		first_seg(lvl->lv)->status &= status_mask;
		if (!set_lv_segment_area_lv(seg, s, lvl->lv, 0,
					    lvl->lv->status)) {
			log_error("Failed to add %s to %s",
				  lvl->lv->name, lv->name);
			goto fail;
		}
		s++;
	}

	/*
	 * FIXME: Failure handling during these points is harder.
	 */
	dm_list_iterate_items(lvl, &meta_lvs)
		lv_set_hidden(lvl->lv);
	dm_list_iterate_items(lvl, &data_lvs)
		lv_set_hidden(lvl->lv);

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv_origin(cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!resume_lv_origin(cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * Now that the 'REBUILD' has made its way to the kernel, we must
	 * remove the flag so that the individual devices are not rebuilt
	 * upon every activation.
	 */
	seg = first_seg(lv);
	for (s = 0; s < seg->area_count; s++) {
		if ((seg_lv(seg, s)->status & LV_REBUILD) ||
		    (seg_metalv(seg, s)->status & LV_REBUILD)) {
			seg_metalv(seg, s)->status &= ~LV_REBUILD;
			seg_lv(seg, s)->status &= ~LV_REBUILD;
			rebuild_flag_cleared = 1;
		}
	}
	if (rebuild_flag_cleared &&
	    (!vg_write(lv->vg) || !vg_commit(lv->vg))) {
		log_error("Failed to clear REBUILD flag for %s/%s components",
			  lv->vg->name, lv->name);
		return 0;
	}

	return 1;

fail:
	/* Cleanly remove newly-allocated LVs that failed insertion attempt */

	dm_list_iterate_items(lvl, &meta_lvs)
		if (!lv_remove(lvl->lv))
			return_0;
	dm_list_iterate_items(lvl, &data_lvs)
		if (!lv_remove(lvl->lv))
			return_0;
	return_0;
}

/*
 * _extract_image_components
 * @seg
 * @idx:  The index in the areas array to remove
 * @extracted_rmeta:  The displaced metadata LV
 * @extracted_rimage:  The displaced data LV
 *
 * This function extracts the image components - setting the respective
 * 'extracted' pointers.  It appends '_extracted' to the LVs' names, so that
 * there are not future conflicts.  It does /not/ commit the results.
 * (IOW, erroring-out requires no unwinding of operations.)
 *
 * This function does /not/ attempt to:
 *    1) shift the 'areas' or 'meta_areas' arrays.
 *       The '[meta_]areas' are left as AREA_UNASSIGNED.
 *    2) Adjust the seg->area_count
 *    3) Name the extracted LVs appropriately (appends '_extracted' to names)
 * These actions must be performed by the caller.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _extract_image_components(struct lv_segment *seg, uint32_t idx,
				     struct logical_volume **extracted_rmeta,
				     struct logical_volume **extracted_rimage)
{
	int len;
	char *tmp_name;
	struct volume_group *vg = seg->lv->vg;
	struct logical_volume *data_lv = seg_lv(seg, idx);
	struct logical_volume *meta_lv = seg_metalv(seg, idx);

	log_very_verbose("Extracting image components %s and %s from %s",
			 data_lv->name, meta_lv->name, seg->lv->name);

	data_lv->status &= ~RAID_IMAGE;
	meta_lv->status &= ~RAID_META;
	lv_set_visible(data_lv);
	lv_set_visible(meta_lv);

	/* release removes data and meta areas */
	remove_seg_from_segs_using_this_lv(data_lv, seg);
	remove_seg_from_segs_using_this_lv(meta_lv, seg);

	seg_type(seg, idx) = AREA_UNASSIGNED;
	seg_metatype(seg, idx) = AREA_UNASSIGNED;

	len = strlen(meta_lv->name) + strlen("_extracted") + 1;
	tmp_name = dm_pool_alloc(vg->vgmem, len);
	if (!tmp_name)
		return_0;
	sprintf(tmp_name, "%s_extracted", meta_lv->name);
	meta_lv->name = tmp_name;

	len = strlen(data_lv->name) + strlen("_extracted") + 1;
	tmp_name = dm_pool_alloc(vg->vgmem, len);
	if (!tmp_name)
		return_0;
	sprintf(tmp_name, "%s_extracted", data_lv->name);
	data_lv->name = tmp_name;

	*extracted_rmeta = meta_lv;
	*extracted_rimage = data_lv;

	return 1;
}

/*
 * _raid_extract_images
 * @lv
 * @new_count:  The absolute count of images (e.g. '2' for a 2-way mirror)
 * @target_pvs:  The list of PVs that are candidates for removal
 * @shift:  If set, use _shift_and_rename_image_components().
 *          Otherwise, leave the [meta_]areas as AREA_UNASSIGNED and
 *          seg->area_count unchanged.
 * @extracted_[meta|data]_lvs:  The LVs removed from the array.  If 'shift'
 *                              is set, then there will likely be name conflicts.
 *
 * This function extracts _both_ portions of the indexed image.  It
 * does /not/ commit the results.  (IOW, erroring-out requires no unwinding
 * of operations.)
 *
 * Returns: 1 on success, 0 on failure
 */
static int _raid_extract_images(struct logical_volume *lv, uint32_t new_count,
			        struct dm_list *target_pvs, int shift,
			        struct dm_list *extracted_meta_lvs,
			        struct dm_list *extracted_data_lvs)
{
	int s, extract, lvl_idx = 0;
	struct lv_list *lvl_array;
	struct lv_segment *seg = first_seg(lv);
	struct logical_volume *rmeta_lv, *rimage_lv;

	extract = seg->area_count - new_count;
	log_verbose("Extracting %u %s from %s/%s", extract,
		    (extract > 1) ? "images" : "image",
		    lv->vg->name, lv->name);

	lvl_array = dm_pool_alloc(lv->vg->vgmem,
				  sizeof(*lvl_array) * extract * 2);
	if (!lvl_array)
		return_0;

	for (s = seg->area_count - 1; (s >= 0) && extract; s--) {
		if (!_lv_is_on_pvs(seg_lv(seg, s), target_pvs) ||
		    !_lv_is_on_pvs(seg_metalv(seg, s), target_pvs))
			continue;
		if (!_raid_in_sync(lv) &&
		    (!seg_is_mirrored(seg) || (s == 0))) {
			log_error("Unable to extract %sRAID image"
				  " while RAID array is not in-sync",
				  seg_is_mirrored(seg) ? "primary " : "");
			return 0;
		}

		if (!_extract_image_components(seg, s, &rmeta_lv, &rimage_lv)) {
			log_error("Failed to extract %s from %s",
				  seg_lv(seg, s)->name, lv->name);
			return 0;
		}

		if (shift && !_shift_and_rename_image_components(seg)) {
			log_error("Failed to shift and rename image components");
			return 0;
		}

		lvl_array[lvl_idx].lv = rmeta_lv;
		lvl_array[lvl_idx + 1].lv = rimage_lv;
		dm_list_add(extracted_meta_lvs, &(lvl_array[lvl_idx++].list));
		dm_list_add(extracted_data_lvs, &(lvl_array[lvl_idx++].list));

		extract--;
	}
	if (extract) {
		log_error("Unable to extract enough images to satisfy request");
		return 0;
	}

	return 1;
}

static int _raid_remove_images(struct logical_volume *lv,
			       uint32_t new_count, struct dm_list *pvs)
{
	struct dm_list removal_list;
	struct lv_list *lvl;

	dm_list_init(&removal_list);

	if (!_raid_extract_images(lv, new_count, pvs, 1,
				 &removal_list, &removal_list)) {
		log_error("Failed to extract images from %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	/* Convert to linear? */
	if ((new_count == 1) && !_raid_remove_top_layer(lv, &removal_list)) {
		log_error("Failed to remove RAID layer after linear conversion");
		return 0;
	}

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(lv->vg->cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	/*
	 * Resume original LV
	 * This also resumes all other sub-lvs (including the extracted)
	 */
	if (!resume_lv(lv->vg->cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * Eliminate the extracted LVs
	 */
	sync_local_dev_names(lv->vg->cmd);
	if (!dm_list_empty(&removal_list)) {
		dm_list_iterate_items(lvl, &removal_list) {
			if (!deactivate_lv(lv->vg->cmd, lvl->lv))
				return_0;
			if (!lv_remove(lvl->lv))
				return_0;
		}

		if (!vg_write(lv->vg) || !vg_commit(lv->vg))
			return_0;
	}

	return 1;
}

/*
 * lv_raid_change_image_count
 * @lv
 * @new_count: The absolute count of images (e.g. '2' for a 2-way mirror)
 * @pvs: The list of PVs that are candidates for removal (or empty list)
 *
 * RAID arrays have 'images' which are composed of two parts, they are:
 *    - 'rimage': The data/parity holding portion
 *    - 'rmeta' : The metadata holding portion (i.e. superblock/bitmap area)
 * This function adds or removes _both_ portions of the image and commits
 * the results.
 *
 * Returns: 1 on success, 0 on failure
 */
int lv_raid_change_image_count(struct logical_volume *lv,
			       uint32_t new_count, struct dm_list *pvs)
{
	uint32_t old_count = lv_raid_image_count(lv);

	if (old_count == new_count) {
		log_error("%s/%s already has image count of %d",
			  lv->vg->name, lv->name, new_count);
		return 1;
	}

	if (old_count > new_count)
		return _raid_remove_images(lv, new_count, pvs);

	return _raid_add_images(lv, new_count, pvs);
}

int lv_raid_split(struct logical_volume *lv, const char *split_name,
		  uint32_t new_count, struct dm_list *splittable_pvs)
{
	const char *old_name;
	struct lv_list *lvl;
	struct dm_list removal_list, data_list;
	struct cmd_context *cmd = lv->vg->cmd;
	uint32_t old_count = lv_raid_image_count(lv);
	struct logical_volume *tracking;
	struct dm_list tracking_pvs;

	dm_list_init(&removal_list);
	dm_list_init(&data_list);

	if ((old_count - new_count) != 1) {
		log_error("Unable to split more than one image from %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!seg_is_mirrored(first_seg(lv))) {
		log_error("Unable to split logical volume of segment type, %s",
			  first_seg(lv)->segtype->name);
		return 0;
	}

	if (find_lv_in_vg(lv->vg, split_name)) {
		log_error("Logical Volume \"%s\" already exists in %s",
			  split_name, lv->vg->name);
		return 0;
	}

	if (!_raid_in_sync(lv)) {
		log_error("Unable to split %s/%s while it is not in-sync.",
			  lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * We only allow a split while there is tracking if it is to
	 * complete the split of the tracking sub-LV
	 */
	if (_lv_is_raid_with_tracking(lv, &tracking)) {
		if (!_lv_is_on_pvs(tracking, splittable_pvs)) {
			log_error("Unable to split additional image from %s "
				  "while tracking changes for %s",
				  lv->name, tracking->name);
			return 0;
		} else {
			/* Ensure we only split the tracking image */
			dm_list_init(&tracking_pvs);
			splittable_pvs = &tracking_pvs;
			if (!_get_pv_list_for_lv(tracking, splittable_pvs))
				return_0;
		}
	}

	if (!_raid_extract_images(lv, new_count, splittable_pvs, 1,
				 &removal_list, &data_list)) {
		log_error("Failed to extract images from %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	/* Convert to linear? */
	if ((new_count == 1) && !_raid_remove_top_layer(lv, &removal_list)) {
		log_error("Failed to remove RAID layer after linear conversion");
		return 0;
	}

	/* Get first item */
	dm_list_iterate_items(lvl, &data_list)
		break;

	old_name = lvl->lv->name;
	lvl->lv->name = split_name;

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	/*
	 * Resume original LV
	 * This also resumes all other sub-lvs (including the extracted)
	 */
	if (!resume_lv(cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	/* Recycle newly split LV so it is properly renamed */
	if (!suspend_lv(cmd, lvl->lv) || !resume_lv(cmd, lvl->lv)) {
		log_error("Failed to rename %s to %s after committing changes",
			  old_name, split_name);
		return 0;
	}

	/*
	 * Eliminate the residual LVs
	 */
	dm_list_iterate_items(lvl, &removal_list) {
		if (!deactivate_lv(cmd, lvl->lv))
			return_0;

		if (!lv_remove(lvl->lv))
			return_0;
	}

	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	return 1;
}

/*
 * lv_raid_split_and_track
 * @lv
 * @splittable_pvs
 *
 * Only allows a single image to be split while tracking.  The image
 * never actually leaves the mirror.  It is simply made visible.  This
 * action triggers two things: 1) users are able to access the (data) image
 * and 2) lower layers replace images marked with a visible flag with
 * error targets.
 *
 * Returns: 1 on success, 0 on error
 */
int lv_raid_split_and_track(struct logical_volume *lv,
			    struct dm_list *splittable_pvs)
{
	int s;
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_mirrored(seg)) {
		log_error("Unable to split images from non-mirrored RAID");
		return 0;
	}

	if (!_raid_in_sync(lv)) {
		log_error("Unable to split image from %s/%s while not in-sync",
			  lv->vg->name, lv->name);
		return 0;
	}

	/* Cannot track two split images at once */
	if (lv_is_raid_with_tracking(lv)) {
		log_error("Cannot track more than one split image at a time");
		return 0;
	}

	for (s = seg->area_count - 1; s >= 0; s--) {
		if (!_lv_is_on_pvs(seg_lv(seg, s), splittable_pvs))
			continue;
		lv_set_visible(seg_lv(seg, s));
		seg_lv(seg, s)->status &= ~LVM_WRITE;
		break;
	}

	if (s >= seg->area_count) {
		log_error("Unable to find image to satisfy request");
		return 0;
	}

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(lv->vg->cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	log_print("%s split from %s for read-only purposes.",
		  seg_lv(seg, s)->name, lv->name);

	/* Resume original LV */
	if (!resume_lv(lv->vg->cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	/* Activate the split (and tracking) LV */
	if (!_activate_sublv_preserving_excl(lv, seg_lv(seg, s)))
		return 0;

	log_print("Use 'lvconvert --merge %s/%s' to merge back into %s",
		  lv->vg->name, seg_lv(seg, s)->name, lv->name);
	return 1;
}

int lv_raid_merge(struct logical_volume *image_lv)
{
	uint32_t s;
	char *p, *lv_name;
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct logical_volume *meta_lv = NULL;
	struct lv_segment *seg;
	struct volume_group *vg = image_lv->vg;

	lv_name = dm_pool_strdup(vg->vgmem, image_lv->name);
	if (!lv_name)
		return_0;

	if (!(p = strstr(lv_name, "_rimage_"))) {
		log_error("Unable to merge non-mirror image %s/%s",
			  vg->name, image_lv->name);
		return 0;
	}
	*p = '\0'; /* lv_name is now that of top-level RAID */

	if (image_lv->status & LVM_WRITE) {
		log_error("%s/%s is not read-only - refusing to merge",
			  vg->name, image_lv->name);
		return 0;
	}

	if (!(lvl = find_lv_in_vg(vg, lv_name))) {
		log_error("Unable to find containing RAID array for %s/%s",
			  vg->name, image_lv->name);
		return 0;
	}
	lv = lvl->lv;
	seg = first_seg(lv);
	for (s = 0; s < seg->area_count; s++) {
		if (seg_lv(seg, s) == image_lv) {
			meta_lv = seg_metalv(seg, s);
		}
	}
	if (!meta_lv)
		return_0;

	if (!deactivate_lv(vg->cmd, meta_lv)) {
		log_error("Failed to deactivate %s", meta_lv->name);
		return 0;
	}

	if (!deactivate_lv(vg->cmd, image_lv)) {
		log_error("Failed to deactivate %s/%s before merging",
			  vg->name, image_lv->name);
		return 0;
	}
	lv_set_hidden(image_lv);
	image_lv->status |= (lv->status & LVM_WRITE);
	image_lv->status |= RAID_IMAGE;

	if (!vg_write(vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, vg->name);
		return 0;
	}

	if (!suspend_lv(vg->cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  vg->name, lv->name);
		return 0;
	}

	if (!vg_commit(vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, vg->name);
		return 0;
	}

	if (!resume_lv(vg->cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  vg->name, lv->name);
		return 0;
	}

	log_print("%s/%s successfully merged back into %s/%s",
		  vg->name, image_lv->name,
		  vg->name, lv->name);
	return 1;
}

static int _convert_mirror_to_raid1(struct logical_volume *lv,
				    const struct segment_type *new_segtype)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);
	struct lv_list lvl_array[seg->area_count], *lvl;
	struct dm_list meta_lvs;
	struct lv_segment_area *meta_areas;

	dm_list_init(&meta_lvs);

	if (!_raid_in_sync(lv)) {
		log_error("Unable to convert %s/%s while it is not in-sync",
			  lv->vg->name, lv->name);
		return 0;
	}

	meta_areas = dm_pool_zalloc(lv->vg->vgmem,
				    lv_mirror_count(lv) * sizeof(*meta_areas));
	if (!meta_areas) {
		log_error("Failed to allocate memory");
		return 0;
	}

	for (s = 0; s < seg->area_count; s++) {
		log_debug("Allocating new metadata LV for %s",
			  seg_lv(seg, s)->name);
		if (!_alloc_rmeta_for_lv(seg_lv(seg, s), &(lvl_array[s].lv))) {
			log_error("Failed to allocate metadata LV for %s in %s",
				  seg_lv(seg, s)->name, lv->name);
			return 0;
		}
		dm_list_add(&meta_lvs, &(lvl_array[s].list));
	}

	log_debug("Clearing newly allocated metadata LVs");
	if (!_clear_lvs(&meta_lvs)) {
		log_error("Failed to initialize metadata LVs");
		return 0;
	}

	if (seg->log_lv) {
		log_debug("Removing mirror log, %s", seg->log_lv->name);
		if (!remove_mirror_log(lv->vg->cmd, lv, NULL, 0)) {
			log_error("Failed to remove mirror log");
			return 0;
		}
	}

	seg->meta_areas = meta_areas;
	s = 0;

	dm_list_iterate_items(lvl, &meta_lvs) {
		log_debug("Adding %s to %s", lvl->lv->name, lv->name);

		/* Images are known to be in-sync */
		lvl->lv->status &= ~LV_REBUILD;
		first_seg(lvl->lv)->status &= ~LV_REBUILD;
		lv_set_hidden(lvl->lv);

		if (!set_lv_segment_area_lv(seg, s, lvl->lv, 0,
					    lvl->lv->status)) {
			log_error("Failed to add %s to %s",
				  lvl->lv->name, lv->name);
			return 0;
		}
		s++;
	}

	for (s = 0; s < seg->area_count; s++) {
		char *new_name;

		new_name = dm_pool_zalloc(lv->vg->vgmem,
					  strlen(lv->name) +
					  strlen("_rimage_XXn"));
		if (!new_name) {
			log_error("Failed to rename mirror images");
			return 0;
		}

		sprintf(new_name, "%s_rimage_%u", lv->name, s);
		log_debug("Renaming %s to %s", seg_lv(seg, s)->name, new_name);
		seg_lv(seg, s)->name = new_name;
		seg_lv(seg, s)->status &= ~MIRROR_IMAGE;
		seg_lv(seg, s)->status |= RAID_IMAGE;
	}
	init_mirror_in_sync(1);

	log_debug("Setting new segtype for %s", lv->name);
	seg->segtype = new_segtype;
	lv->status &= ~MIRRORED;
	lv->status |= RAID;
	seg->status |= RAID;

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(lv->vg->cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!resume_lv(lv->vg->cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	return 1;
}

/*
 * lv_raid_reshape
 * @lv
 * @new_segtype
 *
 * Convert an LV from one RAID type (or 'mirror' segtype) to another.
 *
 * Returns: 1 on success, 0 on failure
 */
int lv_raid_reshape(struct logical_volume *lv,
		    const struct segment_type *new_segtype)
{
	struct lv_segment *seg = first_seg(lv);

	if (!new_segtype) {
		log_error(INTERNAL_ERROR "New segtype not specified");
		return 0;
	}

	if (!strcmp(seg->segtype->name, "mirror") &&
	    (!strcmp(new_segtype->name, "raid1")))
	    return _convert_mirror_to_raid1(lv, new_segtype);

	log_error("Converting the segment type for %s/%s from %s to %s"
		  " is not yet supported.", lv->vg->name, lv->name,
		  seg->segtype->name, new_segtype->name);
	return 0;
}

/*
 * lv_raid_replace
 * @lv
 * @replace_pvs
 * @allocatable_pvs
 *
 * Replace the specified PVs.
 */
int lv_raid_replace(struct logical_volume *lv,
		    struct dm_list *remove_pvs,
		    struct dm_list *allocate_pvs)
{
	uint32_t s, sd, match_count = 0;
	struct dm_list old_meta_lvs, old_data_lvs;
	struct dm_list new_meta_lvs, new_data_lvs;
	struct lv_segment *raid_seg = first_seg(lv);
	struct lv_list *lvl;
	char *tmp_names[raid_seg->area_count * 2];

	dm_list_init(&old_meta_lvs);
	dm_list_init(&old_data_lvs);
	dm_list_init(&new_meta_lvs);
	dm_list_init(&new_data_lvs);

	/*
	 * How many sub-LVs are being removed?
	 */
	for (s = 0; s < raid_seg->area_count; s++) {
		if ((seg_type(raid_seg, s) == AREA_UNASSIGNED) ||
		    (seg_metatype(raid_seg, s) == AREA_UNASSIGNED)) {
			log_error("Unable to replace RAID images while the "
				  "array has unassigned areas");
			return 0;
		}

		if (_lv_is_on_pvs(seg_lv(raid_seg, s), remove_pvs) ||
		    _lv_is_on_pvs(seg_metalv(raid_seg, s), remove_pvs))
			match_count++;
	}

	if (!match_count) {
		log_verbose("%s/%s does not contain devices specified"
			    " for replacement", lv->vg->name, lv->name);
		return 1;
	} else if (match_count == raid_seg->area_count) {
		log_error("Unable to remove all PVs from %s/%s at once.",
			  lv->vg->name, lv->name);
		return 0;
	} else if (raid_seg->segtype->parity_devs &&
		   (match_count > raid_seg->segtype->parity_devs)) {
		log_error("Unable to replace more than %u PVs from (%s) %s/%s",
			  raid_seg->segtype->parity_devs,
			  raid_seg->segtype->name, lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * Allocate the new image components first
	 * - This makes it easy to avoid all currently used devs
	 * - We can immediately tell if there is enough space
	 *
	 * - We need to change the LV names when we insert them.
	 */
	if (!_alloc_image_components(lv, allocate_pvs, match_count,
				     &new_meta_lvs, &new_data_lvs)) {
		log_error("Failed to allocate replacement images for %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * Remove the old images
	 * - If we did this before the allocate, we wouldn't have to rename
	 *   the allocated images, but it'd be much harder to avoid the right
	 *   PVs during allocation.
	 */
	if (!_raid_extract_images(lv, raid_seg->area_count - match_count,
				  remove_pvs, 0,
				  &old_meta_lvs, &old_data_lvs)) {
		log_error("Failed to remove the specified images from %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * Skip metadata operation normally done to clear the metadata sub-LVs.
	 *
	 * The LV_REBUILD flag is set on the new sub-LVs,
	 * so they will be rebuilt and we don't need to clear the metadata dev.
	 */

	for (s = 0; s < raid_seg->area_count; s++) {
		tmp_names[s] = NULL;
		sd = s + raid_seg->area_count;
		tmp_names[sd] = NULL;

		if ((seg_type(raid_seg, s) == AREA_UNASSIGNED) &&
		    (seg_metatype(raid_seg, s) == AREA_UNASSIGNED)) {
			/* Adjust the new metadata LV name */
			lvl = dm_list_item(dm_list_first(&new_meta_lvs),
					   struct lv_list);
			dm_list_del(&lvl->list);
			tmp_names[s] = dm_pool_alloc(lv->vg->vgmem,
						    strlen(lvl->lv->name) + 1);
			if (!tmp_names[s])
				return_0;
			if (dm_snprintf(tmp_names[s], strlen(lvl->lv->name) + 1,
					"%s_rmeta_%u", lv->name, s) < 0)
				return_0;
			if (!set_lv_segment_area_lv(raid_seg, s, lvl->lv, 0,
						    lvl->lv->status)) {
				log_error("Failed to add %s to %s",
					  lvl->lv->name, lv->name);
				return 0;
			}
			lv_set_hidden(lvl->lv);

			/* Adjust the new data LV name */
			lvl = dm_list_item(dm_list_first(&new_data_lvs),
					   struct lv_list);
			dm_list_del(&lvl->list);
			tmp_names[sd] = dm_pool_alloc(lv->vg->vgmem,
						     strlen(lvl->lv->name) + 1);
			if (!tmp_names[sd])
				return_0;
			if (dm_snprintf(tmp_names[sd], strlen(lvl->lv->name) + 1,
					"%s_rimage_%u", lv->name, s) < 0)
				return_0;
			if (!set_lv_segment_area_lv(raid_seg, s, lvl->lv, 0,
						    lvl->lv->status)) {
				log_error("Failed to add %s to %s",
					  lvl->lv->name, lv->name);
				return 0;
			}
			lv_set_hidden(lvl->lv);
		}
	}

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(lv->vg->cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!resume_lv(lv->vg->cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	dm_list_iterate_items(lvl, &old_meta_lvs) {
		if (!deactivate_lv(lv->vg->cmd, lvl->lv))
			return_0;
		if (!lv_remove(lvl->lv))
			return_0;
	}
	dm_list_iterate_items(lvl, &old_data_lvs) {
		if (!deactivate_lv(lv->vg->cmd, lvl->lv))
			return_0;
		if (!lv_remove(lvl->lv))
			return_0;
	}

	/* Update new sub-LVs to correct name and clear REBUILD flag */
	for (s = 0; s < raid_seg->area_count; s++) {
		sd = s + raid_seg->area_count;
		if (tmp_names[s] && tmp_names[sd]) {
			seg_metalv(raid_seg, s)->name = tmp_names[s];
			seg_lv(raid_seg, s)->name = tmp_names[sd];
			seg_metalv(raid_seg, s)->status &= ~LV_REBUILD;
			seg_lv(raid_seg, s)->status &= ~LV_REBUILD;
		}
	}

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(lv->vg->cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!resume_lv(lv->vg->cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	return 1;
}
