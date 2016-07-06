/*
 * Copyright (C) 2011-2014 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib.h"
#include "archiver.h"
#include "metadata.h"
#include "toolcontext.h"
#include "segtype.h"
#include "display.h"
#include "activate.h"
#include "lv_alloc.h"
#include "lvm-string.h"

static int _check_restriping(uint32_t new_stripes, struct logical_volume *lv)
{
	if (new_stripes && new_stripes != first_seg(lv)->area_count) {
		log_error("Cannot restripe LV %s from %" PRIu32 " to %u stripes during conversion.",
			  display_lvname(lv), first_seg(lv)->area_count, new_stripes);
		return 0;
	}

	return 1;
}

static int _lv_is_raid_with_tracking(const struct logical_volume *lv,
				     struct logical_volume **tracking)
{
	uint32_t s;
	const struct lv_segment *seg = first_seg(lv);

	*tracking = NULL;

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
		if (!activate_lv_excl_local(cmd, sub_lv))
			return_0;
	} else {
		if (!activate_lv(cmd, sub_lv))
			return_0;
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
	dm_percent_t sync_percent;

	if (seg_is_striped(first_seg(lv)))
		return 1;

	if (!lv_raid_percent(lv, &sync_percent)) {
		log_error("Unable to determine sync status of %s/%s.",
			  lv->vg->name, lv->name);
		return 0;
	}
	if (sync_percent == DM_PERCENT_0) {
		/*
		 * FIXME We repeat the status read here to workaround an
		 * unresolved kernel bug when we see 0 even though the
		 * the array is 100% in sync.
		 * https://bugzilla.redhat.com/1210637
		 */
		if (!lv_raid_percent(lv, &sync_percent)) {
			log_error("Unable to determine sync status of %s/%s.",
				  lv->vg->name, lv->name);
			return 0;
		}
		if (sync_percent == DM_PERCENT_100)
			log_warn("WARNING: Sync status for %s is inconsistent.",
				 display_lvname(lv));
	}

	return (sync_percent == DM_PERCENT_100) ? 1 : 0;
}

/*
 * _raid_remove_top_layer
 * @lv
 * @removal_lvs
 *
 * Remove top layer of RAID LV in order to convert to linear.
 * This function makes no on-disk changes.  The residual LVs
 * returned in 'removal_lvs' must be freed by the caller.
 *
 * Returns: 1 on succes, 0 on failure
 */
static int _raid_remove_top_layer(struct logical_volume *lv,
				  struct dm_list *removal_lvs)
{
	struct lv_list *lvl_array, *lvl;
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_mirrored(seg)) {
		log_error(INTERNAL_ERROR
			  "Unable to remove RAID layer from segment type %s",
			  lvseg_name(seg));
		return 0;
	}

	if (seg->area_count != 1) {
		log_error(INTERNAL_ERROR
			  "Unable to remove RAID layer when there"
			  " is more than one sub-lv");
		return 0;
	}

	if (!(lvl_array = dm_pool_alloc(lv->vg->vgmem, 2 * sizeof(*lvl))))
		return_0;

	/* Add last metadata area to removal_lvs */
	lvl_array[0].lv = seg_metalv(seg, 0);
	lv_set_visible(seg_metalv(seg, 0));
	if (!remove_seg_from_segs_using_this_lv(seg_metalv(seg, 0), seg))
		return_0;
	seg_metatype(seg, 0) = AREA_UNASSIGNED;
	dm_list_add(removal_lvs, &(lvl_array[0].list));

	/* Remove RAID layer and add residual LV to removal_lvs*/
	seg_lv(seg, 0)->status &= ~RAID_IMAGE;
	lv_set_visible(seg_lv(seg, 0));
	lvl_array[1].lv = seg_lv(seg, 0);
	dm_list_add(removal_lvs, &(lvl_array[1].list));

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
 *	clear first block of device
 * otherwise:
 *	activate, clear, deactivate
 *
 * Returns: 1 on success, 0 on failure
 */
static int _clear_lv(struct logical_volume *lv)
{
	int was_active = lv_is_active_locally(lv);

	if (test_mode())
		return 1;

	lv->status |= LV_TEMPORARY;
	if (!was_active && !activate_lv_local(lv->vg->cmd, lv)) {
		log_error("Failed to activate localy %s for clearing",
			  lv->name);
		return 0;
	}
	lv->status &= ~LV_TEMPORARY;

	log_verbose("Clearing metadata area of %s/%s",
		    lv->vg->name, lv->name);
	/*
	 * Rather than wiping lv->size, we can simply
	 * wipe the first sector to remove the superblock of any previous
	 * RAID devices.  It is much quicker.
	 */
	if (!wipe_lv(lv, (struct wipe_params) { .do_zero = 1, .zero_sectors = 1 })) {
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
		log_debug_metadata(INTERNAL_ERROR "Empty list of LVs given for clearing");
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

/* Generate raid subvolume name and validate it */
static char *_generate_raid_name(struct logical_volume *lv,
				 const char *suffix, int count)
{
	const char *format = (count >= 0) ? "%s_%s_%u" : "%s_%s";
	size_t len = strlen(lv->name) + strlen(suffix) + ((count >= 0) ? 5 : 2);
	char *name;
	int historical;

	if (!(name = dm_pool_alloc(lv->vg->vgmem, len))) {
		log_error("Failed to allocate new name.");
		return NULL;
	}

	if (dm_snprintf(name, len, format, lv->name, suffix, count) < 0)
		return_NULL;

	if (!validate_name(name)) {
		log_error("New logical volume name \"%s\" is not valid.", name);
		return NULL;
	}

	if (lv_name_is_used_in_vg(lv->vg, name, &historical)) {
		log_error("%sLogical Volume %s already exists in volume group %s.",
			  historical ? "historical " : "", name, lv->vg->name);
		return NULL;
	}

	return name;
}
/*
 * Create an LV of specified type.  Set visible after creation.
 * This function does not make metadata changes.
 */
static struct logical_volume *_alloc_image_component(struct logical_volume *lv,
						     const char *alt_base_name,
						     struct alloc_handle *ah, uint32_t first_area,
						     uint64_t type)
{
	uint64_t status;
	char img_name[NAME_LEN];
	const char *type_suffix;
	struct logical_volume *tmp_lv;
	const struct segment_type *segtype;

	switch (type) {
	case RAID_META:
		type_suffix = "rmeta";
		break;
	case RAID_IMAGE:
		type_suffix = "rimage";
		break;
	default:
		log_error(INTERNAL_ERROR
			  "Bad type provided to _alloc_raid_component.");
		return 0;
	}

	if (dm_snprintf(img_name, sizeof(img_name), "%s_%s_%%d",
			(alt_base_name) ? : lv->name, type_suffix) < 0) {
		log_error("Component name for raid %s is too long.", lv->name);
		return 0;
	}

	status = LVM_READ | LVM_WRITE | LV_REBUILD | type;
	if (!(tmp_lv = lv_create_empty(img_name, NULL, status, ALLOC_INHERIT, lv->vg))) {
		log_error("Failed to allocate new raid component, %s.", img_name);
		return 0;
	}

	if (ah) {
		if (!(segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
			return_0;

		if (!lv_add_segment(ah, first_area, 1, tmp_lv, segtype, 0, status, 0)) {
			log_error("Failed to add segment to LV, %s", img_name);
			return 0;
		}
	}

	lv_set_visible(tmp_lv);

	return tmp_lv;
}

static int _alloc_image_components(struct logical_volume *lv,
				   struct dm_list *pvs, uint32_t count,
				   struct dm_list *new_meta_lvs,
				   struct dm_list *new_data_lvs)
{
	uint32_t s;
	uint32_t region_size;
	uint32_t extents;
	struct lv_segment *seg = first_seg(lv);
	const struct segment_type *segtype;
	struct alloc_handle *ah = NULL;
	struct dm_list *parallel_areas;
	struct lv_list *lvl_array;

	if (!(lvl_array = dm_pool_alloc(lv->vg->vgmem,
					sizeof(*lvl_array) * count * 2)))
		return_0;

	if (!(parallel_areas = build_parallel_areas_from_lv(lv, 0, 1)))
		return_0;

	if (seg_is_linear(seg))
		region_size = get_default_region_size(lv->vg->cmd);
	else
		region_size = seg->region_size;

	if (seg_is_raid(seg))
		segtype = seg->segtype;
	else if (!(segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_RAID1)))
		return_0;

	/*
	 * The number of extents is based on the RAID type.  For RAID1,
	 * each of the rimages is the same size - 'le_count'.  However
	 * for RAID 4/5/6, the stripes add together (NOT including the parity
	 * devices) to equal 'le_count'.  Thus, when we are allocating
	 * individual devies, we must specify how large the individual device
	 * is along with the number we want ('count').
	 */
	if (segtype_is_raid10(segtype)) {
		if (seg->area_count < 2) {
			log_error(INTERNAL_ERROR "LV %s needs at least 2 areas.",
				  display_lvname(lv));
			return 0;
		}
		extents = lv->le_count / (seg->area_count / 2); /* we enforce 2 mirrors right now */
	} else
		extents = (segtype->parity_devs) ?
			   (lv->le_count / (seg->area_count - segtype->parity_devs)) :
			   lv->le_count;

	/* Do we need to allocate any extents? */
	if (pvs && !dm_list_empty(pvs) &&
	    !(ah = allocate_extents(lv->vg, NULL, segtype, 0, count, count,
				    region_size, extents, pvs,
				    lv->alloc, 0, parallel_areas)))
		return_0;

	for (s = 0; s < count; ++s) {
		/*
		 * The allocation areas are grouped together.  First
		 * come the rimage allocated areas, then come the metadata
		 * allocated areas.  Thus, the metadata areas are pulled
		 * from 's + count'.
		 */

		/* new_meta_lvs are optional for raid0 */
		if (new_meta_lvs) {
			if (!(lvl_array[s + count].lv =
			      _alloc_image_component(lv, NULL, ah, s + count, RAID_META))) {
				alloc_destroy(ah);
				return_0;
			}
			dm_list_add(new_meta_lvs, &(lvl_array[s + count].list));
		}

		if (new_data_lvs) {
			if (!(lvl_array[s].lv =
			      _alloc_image_component(lv, NULL, ah, s, RAID_IMAGE))) {
				alloc_destroy(ah);
				return_0;
			}
			dm_list_add(new_data_lvs, &(lvl_array[s].list));
		}
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
			       struct logical_volume **meta_lv,
			       struct dm_list *allocate_pvs)
{
	struct dm_list allocatable_pvs;
	struct alloc_handle *ah;
	struct lv_segment *seg = first_seg(data_lv);
	char *p, base_name[NAME_LEN];

	dm_list_init(&allocatable_pvs);

	if (!allocate_pvs)
		allocate_pvs = &allocatable_pvs;

	if (!seg_is_linear(seg)) {
		log_error(INTERNAL_ERROR "Unable to allocate RAID metadata "
			  "area for non-linear LV, %s", data_lv->name);
		return 0;
	}

	(void) dm_strncpy(base_name, data_lv->name, sizeof(base_name));
	if ((p = strstr(base_name, "_mimage_")))
		*p = '\0';

	if (!get_pv_list_for_lv(data_lv->vg->cmd->mem,
				data_lv, &allocatable_pvs)) {
		log_error("Failed to build list of PVs for %s/%s",
			  data_lv->vg->name, data_lv->name);
		return 0;
	}

	if (!(ah = allocate_extents(data_lv->vg, NULL, seg->segtype, 0, 1, 0,
				    seg->region_size,
				    1 /*RAID_METADATA_AREA_LEN*/,
				    &allocatable_pvs, data_lv->alloc, 0, NULL)))
		return_0;

	if (!(*meta_lv = _alloc_image_component(data_lv, base_name, ah, 0, RAID_META))) {
		alloc_destroy(ah);
		return_0;
	}

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
	struct lv_segment *seg = first_seg(lv);
	struct dm_list meta_lvs, data_lvs;
	struct lv_list *lvl;
	struct lv_segment_area *new_areas;

	if (lv->status & LV_NOTSYNCED) {
		log_error("Can't add image to out-of-sync RAID LV:"
			  " use 'lvchange --resync' first.");
		return 0;
	}

	if (!_raid_in_sync(lv)) {
		log_error("Can't add image to RAID LV that"
			  " is still initializing.");
		return 0;
	}

	if (!archive(lv->vg))
		return_0;

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

		if (!_alloc_rmeta_for_lv(lv, &lvl->lv, NULL))
			return_0;

		dm_list_add(&meta_lvs, &lvl->list);
	} else if (!seg_is_raid(seg)) {
		log_error("Unable to add RAID images to %s of segment type %s",
			  lv->name, lvseg_name(seg));
		return 0;
	}

	if (!_alloc_image_components(lv, pvs, count, &meta_lvs, &data_lvs))
		return_0;

	/*
	 * If linear, we must correct data LV names.  They are off-by-one
	 * because the linear volume hasn't taken its proper name of "_rimage_0"
	 * yet.  This action must be done before '_clear_lvs' because it
	 * commits the LVM metadata before clearing the LVs.
	 */
	if (seg_is_linear(seg)) {
		struct dm_list *l;
		struct lv_list *lvl_tmp;

		dm_list_iterate(l, &data_lvs) {
			if (l == dm_list_last(&data_lvs)) {
				lvl = dm_list_item(l, struct lv_list);
				if (!(lvl->lv->name = _generate_raid_name(lv, "rimage", count)))
					return_0;
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
		seg->region_size = get_default_region_size(lv->vg->cmd);

		/* MD's bitmap is limited to tracking 2^21 regions */
		while (seg->region_size < (lv->size / (1 << 21))) {
			seg->region_size *= 2;
			log_very_verbose("Setting RAID1 region_size to %uS",
					 seg->region_size);
		}
		if (!(seg->segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_RAID1)))
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
					 new_count * sizeof(*new_areas)))) {
		log_error("Allocation of new areas failed.");
		goto fail;
	}
	memcpy(new_areas, seg->areas, seg->area_count * sizeof(*seg->areas));
	seg->areas = new_areas;

	/* Expand meta_areas array */
	if (!(new_areas = dm_pool_zalloc(lv->vg->cmd->mem,
					 new_count * sizeof(*new_areas)))) {
		log_error("Allocation of new meta areas failed.");
		goto fail;
	}
	if (seg->meta_areas)
		memcpy(new_areas, seg->meta_areas,
		       seg->area_count * sizeof(*seg->meta_areas));
	seg->meta_areas = new_areas;
	seg->area_count = new_count;

	/* Add extra meta area when converting from linear */
	s = (old_count == 1) ? 0 : old_count;

	/* Set segment areas for metadata sub_lvs */
	dm_list_iterate_items(lvl, &meta_lvs) {
		log_debug_metadata("Adding %s to %s",
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
		log_debug_metadata("Adding %s to %s",
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

	if (!lv_update_and_reload_origin(lv))
		return_0;

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
	if (rebuild_flag_cleared) {
		if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
			log_error("Failed to clear REBUILD flag for %s/%s components",
				  lv->vg->name, lv->name);
			return 0;
		}
		backup(lv->vg);
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

	return 0;
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
	struct logical_volume *data_lv = seg_lv(seg, idx);
	struct logical_volume *meta_lv = seg_metalv(seg, idx);

	log_very_verbose("Extracting image components %s and %s from %s",
			 data_lv->name, meta_lv->name, seg->lv->name);

	data_lv->status &= ~RAID_IMAGE;
	meta_lv->status &= ~RAID_META;
	lv_set_visible(data_lv);
	lv_set_visible(meta_lv);

	/* release removes data and meta areas */
	if (!remove_seg_from_segs_using_this_lv(data_lv, seg) ||
	    !remove_seg_from_segs_using_this_lv(meta_lv, seg))
		return_0;

	seg_type(seg, idx) = AREA_UNASSIGNED;
	seg_metatype(seg, idx) = AREA_UNASSIGNED;

	if (!(data_lv->name = _generate_raid_name(data_lv, "_extracted", -1)))
		return_0;

	if (!(meta_lv->name = _generate_raid_name(meta_lv, "_extracted", -1)))
		return_0;

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
 *	  Otherwise, leave the [meta_]areas as AREA_UNASSIGNED and
 *	  seg->area_count unchanged.
 * @extracted_[meta|data]_lvs:  The LVs removed from the array.  If 'shift'
 *			      is set, then there will likely be name conflicts.
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
	int ss, s, extract, lvl_idx = 0;
	struct lv_list *lvl_array;
	struct lv_segment *seg = first_seg(lv);
	struct logical_volume *rmeta_lv, *rimage_lv;
	struct segment_type *error_segtype;

	extract = seg->area_count - new_count;
	log_verbose("Extracting %u %s from %s/%s", extract,
		    (extract > 1) ? "images" : "image",
		    lv->vg->name, lv->name);
	if ((int) dm_list_size(target_pvs) < extract) {
		log_error("Unable to remove %d images:  Only %d device%s given.",
			  extract, dm_list_size(target_pvs),
			  (dm_list_size(target_pvs) == 1) ? "" : "s");
		return 0;
	}

	if (!(lvl_array = dm_pool_alloc(lv->vg->vgmem,
					sizeof(*lvl_array) * extract * 2)))
		return_0;

	if (!(error_segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_ERROR)))
		return_0;

	/*
	 * We make two passes over the devices.
	 * - The first pass we look for error LVs
	 * - The second pass we look for PVs that match target_pvs
	 */
	for (ss = (seg->area_count * 2) - 1; (ss >= 0) && extract; ss--) {
		s = ss % seg->area_count;

		if (ss / seg->area_count) {
			/* Conditions for first pass */
			if ((first_seg(seg_lv(seg, s))->segtype != error_segtype) &&
			    (first_seg(seg_metalv(seg, s))->segtype != error_segtype))
				continue;

			if (!dm_list_empty(target_pvs) &&
			    (target_pvs != &lv->vg->pvs)) {
				/*
				 * User has supplied a list of PVs, but we
				 * cannot honor that list because error LVs
				 * must come first.
				 */
				log_error("%s has components with error targets"
					  " that must be removed first: %s.",
					  display_lvname(lv),
					  display_lvname(seg_lv(seg, s)));

				log_error("Try removing the PV list and rerun"
					  " the command.");
				return 0;
			}
			log_debug("LVs with error segments to be removed: %s %s",
				  display_lvname(seg_metalv(seg, s)),
				  display_lvname(seg_lv(seg, s)));
		} else {
			/* Conditions for second pass */
			if (!lv_is_on_pvs(seg_lv(seg, s), target_pvs) &&
			    !lv_is_on_pvs(seg_metalv(seg, s), target_pvs))
				continue;

			if (!_raid_in_sync(lv) &&
			    (!seg_is_mirrored(seg) || (s == 0))) {
				log_error("Unable to extract %sRAID image"
					  " while RAID array is not in-sync",
					  seg_is_mirrored(seg) ? "primary " : "");
				return 0;
			}
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
	struct dm_list removal_lvs;
	struct lv_list *lvl;

	if (!archive(lv->vg))
		return_0;

	dm_list_init(&removal_lvs);

	if (!_raid_extract_images(lv, new_count, pvs, 1,
				 &removal_lvs, &removal_lvs)) {
		log_error("Failed to extract images from %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	/* Convert to linear? */
	if (new_count == 1) {
		if (!_raid_remove_top_layer(lv, &removal_lvs)) {
			log_error("Failed to remove RAID layer"
				  " after linear conversion");
			return 0;
		}
		lv->status &= ~(LV_NOTSYNCED | LV_WRITEMOSTLY);
		first_seg(lv)->writebehind = 0;
	}

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(lv->vg->cmd, lv)) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	/*
	 * We activate the extracted sub-LVs first so they are renamed
	 * and won't conflict with the remaining (possibly shifted)
	 * sub-LVs.
	 */
	dm_list_iterate_items(lvl, &removal_lvs) {
		if (!activate_lv_excl_local(lv->vg->cmd, lvl->lv)) {
			log_error("Failed to resume extracted LVs");
			return 0;
		}
	}

	if (!resume_lv(lv->vg->cmd, lv)) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!sync_local_dev_names(lv->vg->cmd)) {
		log_error("Failed to sync local devices after committing changes for %s.",
			  display_lvname(lv));
		return 0;
	}

	/*
	 * Eliminate the extracted LVs
	 */
	if (!dm_list_empty(&removal_lvs)) {
		dm_list_iterate_items(lvl, &removal_lvs) {
			if (!deactivate_lv(lv->vg->cmd, lvl->lv))
				return_0;
			if (!lv_remove(lvl->lv))
				return_0;
		}

		if (!vg_write(lv->vg) || !vg_commit(lv->vg))
			return_0;
	}

	backup(lv->vg);

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
		log_warn("%s/%s already has image count of %d.",
			 lv->vg->name, lv->name, new_count);
		return 1;
	}

	/*
	 * LV must be either in-active or exclusively active
	 */
	if (lv_is_active(lv_lock_holder(lv)) && vg_is_clustered(lv->vg) &&
	    !lv_is_active_exclusive_locally(lv_lock_holder(lv))) {
		log_error("%s/%s must be active exclusive locally to"
			  " perform this operation.", lv->vg->name, lv->name);
		return 0;
	}

	if (old_count > new_count)
		return _raid_remove_images(lv, new_count, pvs);

	return _raid_add_images(lv, new_count, pvs);
}

int lv_raid_split(struct logical_volume *lv, const char *split_name,
		  uint32_t new_count, struct dm_list *splittable_pvs)
{
	struct lv_list *lvl;
	struct dm_list removal_lvs, data_list;
	struct cmd_context *cmd = lv->vg->cmd;
	uint32_t old_count = lv_raid_image_count(lv);
	struct logical_volume *tracking;
	struct dm_list tracking_pvs;
	int historical;

	dm_list_init(&removal_lvs);
	dm_list_init(&data_list);

	if (is_lockd_type(lv->vg->lock_type)) {
		log_error("Splitting raid image is not allowed with lock_type %s",
			  lv->vg->lock_type);
		return 0;
	}

	if ((old_count - new_count) != 1) {
		log_error("Unable to split more than one image from %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!seg_is_mirrored(first_seg(lv)) ||
	    seg_is_raid10(first_seg(lv))) {
		log_error("Unable to split logical volume of segment type, %s",
			  lvseg_name(first_seg(lv)));
		return 0;
	}

	if (lv_name_is_used_in_vg(lv->vg, split_name, &historical)) {
		log_error("%sLogical Volume \"%s\" already exists in %s",
			  historical ? "historical " : "", split_name, lv->vg->name);
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
		if (!lv_is_on_pvs(tracking, splittable_pvs)) {
			log_error("Unable to split additional image from %s "
				  "while tracking changes for %s",
				  lv->name, tracking->name);
			return 0;
		}

		/* Ensure we only split the tracking image */
		dm_list_init(&tracking_pvs);
		splittable_pvs = &tracking_pvs;
		if (!get_pv_list_for_lv(tracking->vg->cmd->mem,
					tracking, splittable_pvs))
			return_0;
	}

	if (!_raid_extract_images(lv, new_count, splittable_pvs, 1,
				 &removal_lvs, &data_list)) {
		log_error("Failed to extract images from %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	/* Convert to linear? */
	if ((new_count == 1) && !_raid_remove_top_layer(lv, &removal_lvs)) {
		log_error("Failed to remove RAID layer after linear conversion");
		return 0;
	}

	/* Get first item */
	dm_list_iterate_items(lvl, &data_list)
		break;

	lvl->lv->name = split_name;

	if (!vg_write(lv->vg)) {
		log_error("Failed to write changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	if (!suspend_lv(cmd, lv_lock_holder(lv))) {
		log_error("Failed to suspend %s/%s before committing changes",
			  lv->vg->name, lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		log_error("Failed to commit changes to %s in %s",
			  lv->name, lv->vg->name);
		return 0;
	}

	/*
	 * First activate the newly split LV and LVs on the removal list.
	 * This is necessary so that there are no name collisions due to
	 * the original RAID LV having possibly had sub-LVs that have been
	 * shifted and renamed.
	 */
	if (!activate_lv_excl_local(cmd, lvl->lv))
		return_0;

	dm_list_iterate_items(lvl, &removal_lvs)
		if (!activate_lv_excl_local(cmd, lvl->lv))
			return_0;

	if (!resume_lv(cmd, lv_lock_holder(lv))) {
		log_error("Failed to resume %s/%s after committing changes",
			  lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * Since newly split LV is typically already active - we need to call
	 * suspend() and resume() to also rename it.
	 *
	 * TODO: activate should recognize it and avoid these 2 calls
	 */

	/*
	 * Eliminate the residual LVs
	 */
	dm_list_iterate_items(lvl, &removal_lvs) {
		if (!deactivate_lv(cmd, lvl->lv))
			return_0;

		if (!lv_remove(lvl->lv))
			return_0;
	}

	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

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

	for (s = seg->area_count - 1; s >= 0; --s) {
		if (!lv_is_on_pvs(seg_lv(seg, s), splittable_pvs))
			continue;
		lv_set_visible(seg_lv(seg, s));
		seg_lv(seg, s)->status &= ~LVM_WRITE;
		break;
	}

	if (s >= (int) seg->area_count) {
		log_error("Unable to find image to satisfy request");
		return 0;
	}

	if (!lv_update_and_reload(lv))
		return_0;

	log_print_unless_silent("%s split from %s for read-only purposes.",
				seg_lv(seg, s)->name, lv->name);

	/* Activate the split (and tracking) LV */
	if (!_activate_sublv_preserving_excl(lv, seg_lv(seg, s)))
		return_0;

	log_print_unless_silent("Use 'lvconvert --merge %s/%s' to merge back into %s",
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

	if (image_lv->status & LVM_WRITE) {
		log_error("%s is not read-only - refusing to merge.",
			  display_lvname(image_lv));
		return 0;
	}

	if (!(lv_name = dm_pool_strdup(vg->vgmem, image_lv->name)))
		return_0;

	if (!(p = strstr(lv_name, "_rimage_"))) {
		log_error("Unable to merge non-mirror image %s.",
			  display_lvname(image_lv));
		return 0;
	}
	*p = '\0'; /* lv_name is now that of top-level RAID */

	if (!(lvl = find_lv_in_vg(vg, lv_name))) {
		log_error("Unable to find containing RAID array for %s.",
			  display_lvname(image_lv));
		return 0;
	}

	lv = lvl->lv;
	seg = first_seg(lv);
	for (s = 0; s < seg->area_count; ++s)
		if (seg_lv(seg, s) == image_lv)
			meta_lv = seg_metalv(seg, s);

	if (!meta_lv) {
		log_error("Failed to find meta for %s in RAID array %s.",
			  display_lvname(image_lv),
			  display_lvname(lv));
		return 0;
	}

	if (!deactivate_lv(vg->cmd, meta_lv)) {
		log_error("Failed to deactivate %s before merging.",
			  display_lvname(meta_lv));
		return 0;
	}

	if (!deactivate_lv(vg->cmd, image_lv)) {
		log_error("Failed to deactivate %s before merging.",
			  display_lvname(image_lv));
		return 0;
	}
	lv_set_hidden(image_lv);
	image_lv->status |= (lv->status & LVM_WRITE);
	image_lv->status |= RAID_IMAGE;

	if (!lv_update_and_reload(lv))
		return_0;

	log_print_unless_silent("%s/%s successfully merged back into %s/%s",
				vg->name, image_lv->name, vg->name, lv->name);
	return 1;
}

/*
 * General conversion functions
 */
static int _convert_mirror_to_raid1(struct logical_volume *lv,
				    const struct segment_type *new_segtype)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);
	struct lv_list lvl_array[seg->area_count], *lvl;
	struct dm_list meta_lvs;
	struct lv_segment_area *meta_areas;
	char *new_name;

	dm_list_init(&meta_lvs);

	if (!_raid_in_sync(lv)) {
		log_error("Unable to convert %s/%s while it is not in-sync",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!(meta_areas = dm_pool_zalloc(lv->vg->vgmem,
					  lv_mirror_count(lv) * sizeof(*meta_areas)))) {
		log_error("Failed to allocate meta areas memory.");
		return 0;
	}

	if (!archive(lv->vg))
		return_0;

	for (s = 0; s < seg->area_count; s++) {
		log_debug_metadata("Allocating new metadata LV for %s",
				   seg_lv(seg, s)->name);
		if (!_alloc_rmeta_for_lv(seg_lv(seg, s), &(lvl_array[s].lv), NULL)) {
			log_error("Failed to allocate metadata LV for %s in %s",
				  seg_lv(seg, s)->name, lv->name);
			return 0;
		}
		dm_list_add(&meta_lvs, &(lvl_array[s].list));
	}

	log_debug_metadata("Clearing newly allocated metadata LVs");
	if (!_clear_lvs(&meta_lvs)) {
		log_error("Failed to initialize metadata LVs");
		return 0;
	}

	if (seg->log_lv) {
		log_debug_metadata("Removing mirror log, %s", seg->log_lv->name);
		if (!remove_mirror_log(lv->vg->cmd, lv, NULL, 0)) {
			log_error("Failed to remove mirror log");
			return 0;
		}
	}

	seg->meta_areas = meta_areas;
	s = 0;

	dm_list_iterate_items(lvl, &meta_lvs) {
		log_debug_metadata("Adding %s to %s", lvl->lv->name, lv->name);

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

	for (s = 0; s < seg->area_count; ++s) {
		if (!(new_name = _generate_raid_name(lv, "rimage", s)))
			return_0;
		log_debug_metadata("Renaming %s to %s", seg_lv(seg, s)->name, new_name);
		seg_lv(seg, s)->name = new_name;
		seg_lv(seg, s)->status &= ~MIRROR_IMAGE;
		seg_lv(seg, s)->status |= RAID_IMAGE;
	}
	init_mirror_in_sync(1);

	log_debug_metadata("Setting new segtype for %s", lv->name);
	seg->segtype = new_segtype;
	lv->status &= ~MIRROR;
	lv->status &= ~MIRRORED;
	lv->status |= RAID;
	seg->status |= RAID;

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

/* Add new @lvs to @lv at @area_offset */
static int _add_image_component_list(struct lv_segment *seg, int delete_from_list,
				     uint64_t lv_flags, struct dm_list *lvs, uint32_t area_offset)
{
	uint32_t s = area_offset;
	struct lv_list *lvl, *tmp;

	dm_list_iterate_items_safe(lvl, tmp, lvs) {
		if (delete_from_list)
			dm_list_del(&lvl->list);

		if (lv_flags & VISIBLE_LV)
			lv_set_visible(lvl->lv);
		else
			lv_set_hidden(lvl->lv);

		if (lv_flags & LV_REBUILD)
			lvl->lv->status |= LV_REBUILD;
		else
			lvl->lv->status &= ~LV_REBUILD;

		if (!set_lv_segment_area_lv(seg, s++, lvl->lv, 0 /* le */, lvl->lv->status)) {
			log_error("Failed to add sublv %s", lvl->lv->name);
			return 0;
		}
	}

	return 1;
}

/*
 * All areas from LV segments are moved to new
 * segments allocated with area_count=1 for data_lvs.
 */
static int _striped_to_raid0_move_segs_to_raid0_lvs(struct logical_volume *lv,
						    struct dm_list *data_lvs)
{
	uint32_t s = 0, le;
	struct logical_volume *dlv;
	struct lv_segment *seg_from, *seg_new;
	struct lv_list *lvl;
	struct segment_type *segtype;
	uint64_t status;

	if (!(segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	/* Move segment areas across to the N data LVs of the new raid0 LV */
	dm_list_iterate_items(lvl, data_lvs)  {
		dlv = lvl->lv;
		le = 0;
		dm_list_iterate_items(seg_from, &lv->segments) {
			status = RAID | SEG_RAID | (seg_from->status & (LVM_READ | LVM_WRITE));

			/* Allocate a data LV segment with one area for each segment in the striped LV */
			if (!(seg_new = alloc_lv_segment(segtype, dlv,
							 le, seg_from->area_len,
							 status,
							 0 /* stripe_size */, NULL, 1 /* area_count */,
							 seg_from->area_len,
							 0 /* chunk_size */, 0 /* region_size */, 0, NULL)))
				return_0;

			seg_type(seg_new, 0) = AREA_UNASSIGNED;
			dm_list_add(&dlv->segments, &seg_new->list);
			le += seg_from->area_len;

			/* Move the respective area across to our new segment */
			if (!move_lv_segment_area(seg_new, 0, seg_from, s))
				return_0;
		}

		/* Adjust le count and LV size */
		dlv->le_count = le;
		dlv->size = (uint64_t) le * lv->vg->extent_size;
		s++;
	}

	/* Remove the empty segments from the striped LV */
	dm_list_init(&lv->segments);

	return 1;
}

/*
 * Find the smallest area across all the subLV segments at area_le.
 */
static uint32_t _min_sublv_area_at_le(struct lv_segment *seg, uint32_t area_le)
{
	uint32_t s, area_len = ~0U;
	struct lv_segment *seg1;

	/* Find smallest segment of each of the data image LVs at offset area_le */
	for (s = 0; s < seg->area_count; s++) {
		if (!(seg1 = find_seg_by_le(seg_lv(seg, s), area_le))) {
			log_error("Failed to find segment for %s extent %" PRIu32,
				  seg_lv(seg, s)->name, area_le);
			return 0;
		}

		area_len = min(area_len, seg1->len);
	}

	return area_len;
}

/*
 * Split segments in segment LVs in all areas of seg at offset area_le
 */
static int _split_area_lvs_segments(struct lv_segment *seg, uint32_t area_le)
{
	uint32_t s;

	/* Make sure that there's a segment starting at area_le in all data LVs */
	for (s = 0; s < seg->area_count; s++)
		if (area_le < seg_lv(seg, s)->le_count &&
		    !lv_split_segment(seg_lv(seg, s), area_le))
			return_0;

	return 1;
}

static int _alloc_and_add_new_striped_segment(struct logical_volume *lv,
					      uint32_t le, uint32_t area_len,
					      struct dm_list *new_segments)
{
	struct lv_segment *seg, *new_seg;
	struct segment_type *striped_segtype;

	seg = first_seg(lv);

	if (!(striped_segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	/* Allocate a segment with seg->area_count areas */
	if (!(new_seg = alloc_lv_segment(striped_segtype, lv, le, area_len * seg->area_count,
					 seg->status & ~RAID,
					 seg->stripe_size, NULL, seg->area_count,
					 area_len, seg->chunk_size, 0, 0, NULL)))
		return_0;

	dm_list_add(new_segments, &new_seg->list);

	return 1;
}

static int _extract_image_component_error_seg(struct lv_segment *seg,
					      uint64_t type, uint32_t idx,
					      struct logical_volume **extracted_lv,
					      int set_error_seg)
{
	struct logical_volume *lv;

	switch (type) {
		case RAID_META:
			lv = seg_metalv(seg, idx);
			seg_metalv(seg, idx) = NULL;
			seg_metatype(seg, idx) = AREA_UNASSIGNED;
			break;
		case RAID_IMAGE:
			lv = seg_lv(seg, idx);
			seg_lv(seg, idx) = NULL;
			seg_type(seg, idx) = AREA_UNASSIGNED;
			break;
		default:
			log_error(INTERNAL_ERROR "Bad type provided to %s.", __func__);
			return 0;
	}

	log_very_verbose("Extracting image component %s from %s", lv->name, lvseg_name(seg));
	lv->status &= ~(type | RAID);
	lv_set_visible(lv);

	/* remove reference from seg to lv */
	if (!remove_seg_from_segs_using_this_lv(lv, seg))
		return_0;

	if (!(lv->name = _generate_raid_name(lv, "extracted_", -1)))
		return_0;

	if (set_error_seg && !replace_lv_with_error_segment(lv))
		return_0;

	*extracted_lv = lv;

	return 1;
}

/*
 * Extract all sub LVs of type from seg starting at idx excluding end and
 * put them on removal_lvs setting mappings to "error" if error_seg.
 */
static int _extract_image_component_sublist(struct lv_segment *seg,
					    uint64_t type, uint32_t idx, uint32_t end,
					    struct dm_list *removal_lvs,
					    int error_seg)
{
	uint32_t s;
	struct lv_list *lvl;

	if (!(lvl = dm_pool_alloc(seg_lv(seg, idx)->vg->vgmem, sizeof(*lvl) * (end - idx))))
		return_0;

	for (s = idx; s < end; s++) {
		if (!_extract_image_component_error_seg(seg, type, s, &lvl->lv, error_seg))
			return 0;

		dm_list_add(removal_lvs, &lvl->list);
		lvl++;
	}

	if (!idx && end == seg->area_count) {
		if (type == RAID_IMAGE)
			seg->areas = NULL;
		else
			seg->meta_areas = NULL;
	}

	return 1;
}

/* Extract all sub LVs of type from seg starting with idx and put them on removal_Lvs */
static int _extract_image_component_list(struct lv_segment *seg,
					 uint64_t type, uint32_t idx,
					 struct dm_list *removal_lvs)
{
	return _extract_image_component_sublist(seg, type, idx, seg->area_count, removal_lvs, 1);
}

/*
 * All areas from lv image component LV's segments are
 * being split at "striped" compatible boundaries and
 * moved to allocated new_segments.
 *
 * The data component LVs are mapped to an
 * error target and linked to removal_lvs for disposal
 * by the caller.
 */
static int _raid0_to_striped_retrieve_segments_and_lvs(struct logical_volume *lv,
						       struct dm_list *removal_lvs)
{
	uint32_t s, area_le, area_len, le;
	struct lv_segment *data_seg, *seg, *seg_to;
	struct dm_list new_segments;

	seg = first_seg(lv);

	dm_list_init(&new_segments);

	/*
	 * Walk all segments of all data LVs splitting them up at proper boundaries
	 * and create the number of new striped segments we need to move them across
	 */
	area_le = le = 0;
	while (le < lv->le_count) {
		if (!(area_len = _min_sublv_area_at_le(seg, area_le)))
			return_0;
		area_le += area_len;

		if (!_split_area_lvs_segments(seg, area_le) ||
		    !_alloc_and_add_new_striped_segment(lv, le, area_len, &new_segments))
			return_0;

		le = area_le * seg->area_count;
	}

	/* Now move the prepared split areas across to the new segments */
	area_le = 0;
	dm_list_iterate_items(seg_to, &new_segments) {
		for (s = 0; s < seg->area_count; s++) {
			if (!(data_seg = find_seg_by_le(seg_lv(seg, s), area_le))) {
				log_error("Failed to find segment for %s extent %" PRIu32,
					  seg_lv(seg, s)->name, area_le);
				return 0;
			}

			/* Move the respective area across to our new segments area */
			if (!move_lv_segment_area(seg_to, s, data_seg, 0))
				return_0;
		}

		/* Presumes all data LVs have equal size */
		area_le += data_seg->len;
	}

	/* Extract any metadata LVs and the empty data LVs for disposal by the caller */
	if (!_extract_image_component_list(seg, RAID_IMAGE, 0, removal_lvs))
		return_0;

	/*
	 * Remove the one segment holding the image component areas
	 * from the top-level LV, then add the new segments to it
	 */
	dm_list_del(&seg->list);
	dm_list_splice(&lv->segments, &new_segments);

	return 1;
}

/*
 * Deactivate and remove the LVs on removal_lvs list from vg.
 */
static int _deactivate_and_remove_lvs(struct volume_group *vg, struct dm_list *removal_lvs)
{
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, removal_lvs)
		if (!deactivate_lv(vg->cmd, lvl->lv) ||
		    !lv_remove(lvl->lv))
			return_0;

	return 1;
}

/* FIXME Move this out */
/* Write, commit and optionally backup metadata of vg */
static int _vg_write_commit_backup(struct volume_group *vg)
{
	if (!vg_write(vg) || !vg_commit(vg)) {
		log_error("Failed to commit VG %s metadata.", vg->name);
		return 0;
	}

	if (!backup(vg))
		log_warn("WARNING: Backup of VG %s metadata failed. Continuing.", vg->name);

	return 1;
}

/*
 * Eliminate the extracted LVs on @removal_lvs from @vg incl. vg write, commit and backup 
 */
static int _eliminate_extracted_lvs_optional_write_vg(struct volume_group *vg,
						      struct dm_list *removal_lvs,
						      int vg_write_requested)
{
	if (!removal_lvs || dm_list_empty(removal_lvs))
		return 1;

	if (!_deactivate_and_remove_lvs(vg, removal_lvs))
		return_0;

	/* Wait for events following any deactivation. */
	if (!sync_local_dev_names(vg->cmd)) {
		log_error("Failed to sync local devices after removing %u LVs in VG %s.",
			  dm_list_size(removal_lvs), vg->name);
		return 0;
	}

	dm_list_init(removal_lvs);

	if (vg_write_requested && !_vg_write_commit_backup(vg))
		return_0;

	return 1;
}

static int _eliminate_extracted_lvs(struct volume_group *vg, struct dm_list *removal_lvs)
{
	return _eliminate_extracted_lvs_optional_write_vg(vg, removal_lvs, 1);
}

static int _avoid_pvs_of_lv(struct logical_volume *lv, void *data)
{
	struct dm_list *allocate_pvs = (struct dm_list *) data;
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, allocate_pvs)
		if (!lv_is_partial(lv) && lv_is_on_pv(lv, pvl->pv))
			pvl->pv->status |= PV_ALLOCATION_PROHIBITED;

	return 1;
}

/*
 * Prevent any PVs holding other image components of @lv from being used for allocation
 * by setting the internal PV_ALLOCATION_PROHIBITED flag to use it to avoid generating
 * pv maps for those PVs.
 */
static int _avoid_pvs_with_other_images_of_lv(struct logical_volume *lv, struct dm_list *allocate_pvs)
{
	return for_each_sub_lv(lv, _avoid_pvs_of_lv, allocate_pvs);
}

static void _clear_allocation_prohibited(struct dm_list *pvs)
{
	struct pv_list *pvl;

	if (pvs)
		dm_list_iterate_items(pvl, pvs)
			pvl->pv->status &= ~PV_ALLOCATION_PROHIBITED;
}

/*
 * Allocate metadata devs for all @new_data_devs and link them to list @new_meta_lvs
 */
static int _alloc_rmeta_devs_for_rimage_devs(struct logical_volume *lv,
					     struct dm_list *new_data_lvs,
					     struct dm_list *new_meta_lvs,
					     struct dm_list *allocate_pvs)
{
	uint32_t a = 0, raid_devs = dm_list_size(new_data_lvs);
	struct lv_list *lvl, *lvl1, *lvl_array;

	if (!raid_devs)
		return_0;

	if (!(lvl_array = dm_pool_zalloc(lv->vg->vgmem, raid_devs * sizeof(*lvl_array))))
		return_0;

	dm_list_iterate_items(lvl, new_data_lvs) {
		log_debug_metadata("Allocating new metadata LV for %s", lvl->lv->name);

		if (!_alloc_rmeta_for_lv(lvl->lv, &lvl_array[a].lv, allocate_pvs)) {
			log_error("Failed to allocate metadata LV for %s in %s",
				  lvl->lv->name, lv->vg->name);
			return 0;
		}

		dm_list_add(new_meta_lvs, &lvl_array[a++].list);
		
		dm_list_iterate_items(lvl1, new_meta_lvs)
			if (!_avoid_pvs_with_other_images_of_lv(lvl1->lv, allocate_pvs))
				return_0;
	}

	_clear_allocation_prohibited(allocate_pvs);

	return 1;
}

/*
 * Allocate metadata devs for all data devs of an LV
 */
static int _alloc_rmeta_devs_for_lv(struct logical_volume *lv,
				    struct dm_list *meta_lvs,
				    struct dm_list *allocate_pvs)
{
	uint32_t s;
	struct lv_list *lvl_array;
	struct dm_list data_lvs;
	struct lv_segment *seg = first_seg(lv);

	dm_list_init(&data_lvs);

	if (!(seg->meta_areas = dm_pool_zalloc(lv->vg->vgmem, seg->area_count * sizeof(*seg->meta_areas))))
		return 0;

	if (!(lvl_array = dm_pool_alloc(lv->vg->vgmem, seg->area_count * sizeof(*lvl_array))))
		return_0;

	for (s = 0; s < seg->area_count; s++) {
		lvl_array[s].lv = seg_lv(seg, s);
		dm_list_add(&data_lvs, &lvl_array[s].list);
	}

	if (!_alloc_rmeta_devs_for_rimage_devs(lv, &data_lvs, meta_lvs, allocate_pvs)) {
		log_error("Failed to allocate metadata LVs for %s", lv->name);
		return 0;
	}

	return 1;
}

/*
 * Add metadata areas to raid0
 */
static int _alloc_and_add_rmeta_devs_for_lv(struct logical_volume *lv, struct dm_list *allocate_pvs)
{
	struct lv_segment *seg = first_seg(lv);
	struct dm_list meta_lvs;

	dm_list_init(&meta_lvs);

	log_debug_metadata("Allocating metadata LVs for %s", display_lvname(lv));
	if (!_alloc_rmeta_devs_for_lv(lv, &meta_lvs, allocate_pvs)) {
		log_error("Failed to allocate metadata LVs for %s", display_lvname(lv));
		return_0;
	}

	/* Metadata LVs must be cleared before being added to the array */
	log_debug_metadata("Clearing newly allocated metadata LVs for %s", display_lvname(lv));
	if (!_clear_lvs(&meta_lvs)) {
		log_error("Failed to initialize metadata LVs for %s", display_lvname(lv));
		return_0;
	}

	/* Set segment areas for metadata sub_lvs */
	log_debug_metadata("Adding newly allocated metadata LVs to %s", display_lvname(lv));
	if (!_add_image_component_list(seg, 1, 0, &meta_lvs, 0)) {
		log_error("Failed to add newly allocated metadata LVs to %s", display_lvname(lv));
		return_0;
	}

	return 1;
}

/*
 * Add/remove metadata areas to/from raid0
 */
static int _raid0_add_or_remove_metadata_lvs(struct logical_volume *lv,
					     int update_and_reload,
					     struct dm_list *allocate_pvs,
					     struct dm_list *removal_lvs)
{
	uint64_t new_raid_type_flag;
	struct lv_segment *seg = first_seg(lv);

	if (removal_lvs) {
		if (seg->meta_areas) {
			if (!_extract_image_component_list(seg, RAID_META, 0, removal_lvs))
				return_0;
			seg->meta_areas = NULL;
		}
		new_raid_type_flag = SEG_RAID0;
	} else {
		if (!_alloc_and_add_rmeta_devs_for_lv(lv, allocate_pvs))
			return 0;

		new_raid_type_flag = SEG_RAID0_META;
	}

	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, new_raid_type_flag)))
		return_0;

	if (update_and_reload) {
		if (!lv_update_and_reload_origin(lv))
			return_0;

		/* If any residual LVs, eliminate them, write VG, commit it and take a backup */
		return _eliminate_extracted_lvs(lv->vg, removal_lvs);
	}

	return 1;
}

static int _raid0_meta_change_wrapper(struct logical_volume *lv,
				     const struct segment_type *new_segtype,
				     uint32_t new_stripes,
				     int yes, int force, int alloc_metadata_devs,
				     struct dm_list *allocate_pvs)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (!_check_restriping(new_stripes, lv))
		return_0;

	if (!archive(lv->vg))
		return_0;

	if (alloc_metadata_devs)
		return _raid0_add_or_remove_metadata_lvs(lv, 1, allocate_pvs, NULL);
	else
		return _raid0_add_or_remove_metadata_lvs(lv, 1, allocate_pvs, &removal_lvs);
}



/*
 * Convert a RAID0 set to striped
 */
static int _convert_raid0_to_striped(struct logical_volume *lv,
				     int update_and_reload,
				     struct dm_list *removal_lvs)
{
	struct lv_segment *seg = first_seg(lv);

	/* Remove metadata devices */
	if (seg_is_raid0_meta(seg) &&
	    !_raid0_add_or_remove_metadata_lvs(lv, 0 /* update_and_reload */, NULL, removal_lvs))
		return_0;

	/* Move the AREA_PV areas across to new top-level segments of type "striped" */
	if (!_raid0_to_striped_retrieve_segments_and_lvs(lv, removal_lvs)) {
		log_error("Failed to retrieve raid0 segments from %s.", lv->name);
		return 0;
	}

	lv->status &= ~RAID;

	if (!(seg->segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	if (update_and_reload) {
		if (!lv_update_and_reload(lv))
			return_0;

		/* Eliminate the residual LVs, write VG, commit it and take a backup */
		return _eliminate_extracted_lvs(lv->vg, removal_lvs);
	} 

	return 1;
}

/*
 * Inserts hidden LVs for all segments and the parallel areas in lv and moves 
 * given segments and areas across.
 *
 * Optionally updates metadata and reloads mappings.
 */
static struct lv_segment *_convert_striped_to_raid0(struct logical_volume *lv,
						    int alloc_metadata_devs,
						    int update_and_reload,
						    struct dm_list *allocate_pvs)
{
	uint32_t area_count, area_len = 0, stripe_size;
	struct lv_segment *seg, *raid0_seg;
	struct segment_type *segtype;
	struct dm_list data_lvs;

	dm_list_iterate_items(seg, &lv->segments)
		area_len += seg->area_len;

	seg = first_seg(lv);
	stripe_size = seg->stripe_size;
	area_count = seg->area_count;

	/* Check for not (yet) supported varying area_count on multi-segment striped LVs */
	if (!lv_has_constant_stripes(lv)) {
		log_error("Cannot convert striped LV %s with varying stripe count to raid0",
			  display_lvname(lv));
		return NULL;
	}

	if (!is_power_of_2(seg->stripe_size)) {
		log_error("Cannot convert striped LV %s with non-power of 2 stripe size %u",
			  display_lvname(lv), seg->stripe_size);
		// log_error("Please use \"lvconvert --duplicate ...\"");
		return NULL;
	}

	if (!(segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0)))
		return_NULL;

	/* Allocate empty rimage components */
	dm_list_init(&data_lvs);
	if (!_alloc_image_components(lv, NULL, area_count, NULL, &data_lvs)) {
		log_error("Failed to allocate empty image components for raid0 LV %s.",
			  display_lvname(lv));
		return NULL;
	}

	/* Move the AREA_PV areas across to the new rimage components; empties lv->segments */
	if (!_striped_to_raid0_move_segs_to_raid0_lvs(lv, &data_lvs)) {
		log_error("Failed to insert linear LVs underneath %s.", display_lvname(lv));
		return NULL;
	}

	/*
	 * Allocate single segment to hold the image component
	 * areas based on the first data LVs properties derived
	 * from the first new raid0 LVs first segment
	 */
	seg = first_seg(dm_list_item(dm_list_first(&data_lvs), struct lv_list)->lv);
	if (!(raid0_seg = alloc_lv_segment(segtype, lv,
					   0 /* le */, lv->le_count /* len */,
					   seg->status | SEG_RAID,
					   stripe_size, NULL /* log_lv */,
					   area_count, area_len,
					   0 /* chunk_size */,
					   0 /* seg->region_size */, 0u /* extents_copied */ ,
					   NULL /* pvmove_source_seg */))) {
		log_error("Failed to allocate new raid0 segement for LV %s.", display_lvname(lv));
		return NULL;
	}

	/* Add new single raid0 segment to emptied LV segments list */
	dm_list_add(&lv->segments, &raid0_seg->list);

	/* Add data LVs to the top-level LVs segment; resets LV_REBUILD flag on them */
	if (!_add_image_component_list(raid0_seg, 1, 0, &data_lvs, 0))
		return NULL;

	lv->status |= RAID;

	/* Allocate metadata LVs if requested */
	if (alloc_metadata_devs && !_raid0_add_or_remove_metadata_lvs(lv, 0, allocate_pvs, NULL))
		return NULL;

	if (update_and_reload && !lv_update_and_reload(lv))
		return NULL;

	return raid0_seg;
}

/************************************************/

/*
 * Individual takeover functions.
 */
#define TAKEOVER_FN_ARGS			\
	struct logical_volume *lv,		\
	const struct segment_type *new_segtype,	\
	int yes,				\
	int force,				\
	unsigned new_image_count,		\
	const unsigned new_stripes,		\
	unsigned new_stripe_size,		\
	struct dm_list *allocate_pvs

typedef int (*takeover_fn_t)(TAKEOVER_FN_ARGS);

/*
 * Common takeover functions.
 */
static int _takeover_noop(TAKEOVER_FN_ARGS)
{
	log_error("Logical volume %s is already of requested type %s.",
		  display_lvname(lv), lvseg_name(first_seg(lv)));

	return 0;
}

static int _takeover_unsupported(TAKEOVER_FN_ARGS)
{
	log_error("Converting the segment type for %s from %s to %s is not supported.",
		  display_lvname(lv), lvseg_name(first_seg(lv)),
		  (segtype_is_striped(new_segtype) && !segtype_is_any_raid0(new_segtype) &&
		   (new_stripes == 1)) ? SEG_TYPE_NAME_LINEAR : new_segtype->name);

	return 0;
}

/*
 * Will this particular takeover combination be possible?
 */
static int _takeover_not_possible(takeover_fn_t takeover_fn)
{
	if (takeover_fn == _takeover_noop || takeover_fn == _takeover_unsupported)
		return 1;

	return 0;
}

static int _takeover_unsupported_yet(const struct logical_volume *lv, const unsigned new_stripes, const struct segment_type *new_segtype)
{
	log_error("Converting the segment type for %s from %s to %s is not supported yet.",
		  display_lvname(lv), lvseg_name(first_seg(lv)),
		  (segtype_is_striped(new_segtype) && !segtype_is_any_raid0(new_segtype) &&
		   (new_stripes == 1)) ? SEG_TYPE_NAME_LINEAR : new_segtype->name);

	return 0;
}

/*
 * Customised takeover functions
 */
static int _takeover_from_linear_to_raid0(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_linear_to_raid1(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_linear_to_raid10(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_linear_to_raid45(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_mirrored_to_raid0(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_mirrored_to_raid0_meta(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_mirrored_to_raid1(TAKEOVER_FN_ARGS)
{
	return _convert_mirror_to_raid1(lv, new_segtype);
}

static int _takeover_from_mirrored_to_raid10(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_mirrored_to_raid45(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_to_linear(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_to_mirrored(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}
static int _takeover_from_raid0_to_raid0_meta(TAKEOVER_FN_ARGS)
{
	if (!_raid0_meta_change_wrapper(lv, new_segtype, new_stripes, yes, force, 1, allocate_pvs))
		return_0;

	return 1;
}

static int _takeover_from_raid0_to_raid1(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_to_raid10(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_to_raid45(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_to_raid6(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _raid0_to_striped_wrapper(struct logical_volume *lv,
				     const struct segment_type *new_segtype,
				     uint32_t new_stripes,
				     int yes, int force,
				     struct dm_list *allocate_pvs)
{
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	if (!_check_restriping(new_stripes, lv))
		return_0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* FIXME update_and_reload is only needed if the LV is already active */
	/* FIXME Some of the validation in here needs moving before the archiving */
	if (!_convert_raid0_to_striped(lv, 1 /* update_and_reload */, &removal_lvs))
		return_0;

	return 1;
}

static int _takeover_from_raid0_to_striped(TAKEOVER_FN_ARGS)
{
	if (!_raid0_to_striped_wrapper(lv, new_segtype, new_stripes, yes, force, allocate_pvs))
		return_0;

	return 1;
}

static int _takeover_from_raid0_meta_to_linear(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_meta_to_mirrored(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_meta_to_raid0(TAKEOVER_FN_ARGS)
{
	if (!_raid0_meta_change_wrapper(lv, new_segtype, new_stripes, yes, force, 0, allocate_pvs))
		return_0;

	return 1;
}

static int _takeover_from_raid0_meta_to_raid1(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_meta_to_raid10(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_meta_to_raid45(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_meta_to_raid6(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid0_meta_to_striped(TAKEOVER_FN_ARGS)
{
	if (!_raid0_to_striped_wrapper(lv, new_segtype, new_stripes, yes, force, allocate_pvs))
		return_0;

	return 1;
}

static int _takeover_from_raid1_to_linear(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid1_to_mirrored(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid1_to_raid0(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid1_to_raid0_meta(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid1_to_raid1(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid1_to_raid10(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid1_to_raid45(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid1_to_striped(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid45_to_linear(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid45_to_mirrored(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid45_to_raid0(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid45_to_raid0_meta(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid45_to_raid1(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid45_to_raid54(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid45_to_raid6(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid45_to_striped(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid6_to_raid0(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid6_to_raid0_meta(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid6_to_raid45(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid6_to_striped(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _striped_to_raid0_wrapper(struct logical_volume *lv,
				     const struct segment_type *new_segtype,
				     uint32_t new_stripes,
				     int yes, int force, int alloc_metadata_devs,
				     struct dm_list *allocate_pvs)
{
	if (!_check_restriping(new_stripes, lv))
		return_0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* FIXME update_and_reload is only needed if the LV is already active */
	/* FIXME Some of the validation in here needs moving before the archiving */
	if (!_convert_striped_to_raid0(lv, alloc_metadata_devs, 1 /* update_and_reload */, allocate_pvs))
		return_0;

	return 1;
}

static int _takeover_from_striped_to_raid0(TAKEOVER_FN_ARGS)
{
	if (!_striped_to_raid0_wrapper(lv, new_segtype, new_stripes, yes, force, 0, allocate_pvs))
		return_0;

	return 1;
}

static int _takeover_from_striped_to_raid01(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_striped_to_raid0_meta(TAKEOVER_FN_ARGS)
{
	if (!_striped_to_raid0_wrapper(lv, new_segtype, new_stripes, yes, force, 1, allocate_pvs))
		return_0;

	return 1;
}

static int _takeover_from_striped_to_raid10(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_striped_to_raid45(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_striped_to_raid6(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

/*
static int _takeover_from_raid01_to_raid01(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid01_to_raid10(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid01_to_striped(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid10_to_linear(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid10_to_mirrored(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid10_to_raid0(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid10_to_raid01(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid10_to_raid0_meta(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid10_to_raid1(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid10_to_raid10(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}

static int _takeover_from_raid10_to_striped(TAKEOVER_FN_ARGS)
{
	return _takeover_unsupported_yet(lv, new_stripes, new_segtype);
}
*/

/*
 * Import takeover matrix.
 */
#include "takeover_matrix.h"

static unsigned _segtype_ix(const struct segment_type *segtype, uint32_t area_count)
{
	int i = 2, j;

	/* Linear special case */
	if (segtype_is_striped(segtype) && !segtype_is_any_raid0(segtype)) {
		if (area_count == 1)
			return 0;	/* linear */
		return 1;	/* striped */
	}

	while ((j = _segtype_index[i++]))
		if (segtype->flags & j)
			break;

	return (i - 1);
}

/* Call appropriate takeover function */
static takeover_fn_t _get_takeover_fn(const struct lv_segment *seg, const struct segment_type *new_segtype, unsigned new_image_count)
{
	return _takeover_fns[_segtype_ix(seg->segtype, seg->area_count)][_segtype_ix(new_segtype, new_image_count)];
}

/*
 * Check for maximum number of raid devices.
 * Constrained by kernel MD maximum device limits _and_ dm-raid superblock
 * bitfield constraints.
 */
static int _check_max_raid_devices(uint32_t image_count)
{
	if (image_count > DEFAULT_RAID_MAX_IMAGES) {
		log_error("Unable to handle arrays with more than %u devices",
			  DEFAULT_RAID_MAX_IMAGES);
		return 0;
	}
	return 1;
}

/* Number of data (not parity) rimages */
static uint32_t _data_rimages_count(const struct lv_segment *seg, const uint32_t total_rimages)
{
	return total_rimages - seg->segtype->parity_devs;
}

/*
 * lv_raid_convert
 *
 * Convert an LV from one RAID type (or 'mirror' segtype) to another.
 *
 * Returns: 1 on success, 0 on failure
 */
int lv_raid_convert(struct logical_volume *lv,
		    const struct segment_type *new_segtype,
		    int yes, int force,
		    const unsigned new_stripes,
		    const unsigned new_stripe_size,
		    struct dm_list *allocate_pvs)
{
	struct lv_segment *seg = first_seg(lv);
	uint32_t stripes, stripe_size;
	uint32_t new_image_count = seg->area_count;
	takeover_fn_t takeover_fn;

	if (!new_segtype) {
		log_error(INTERNAL_ERROR "New segtype not specified");
		return 0;
	}

	stripes = new_stripes ?: _data_rimages_count(seg, seg->area_count);

	if (segtype_is_striped(new_segtype))
		new_image_count = stripes;

	if (segtype_is_raid(new_segtype) && !_check_max_raid_devices(new_image_count))
		return_0;

	/* FIXME Ensure caller does *not* set wrong default value! */
	/* Define new stripe size if not passed in */
	stripe_size = new_stripe_size ?: seg->stripe_size;

	takeover_fn = _get_takeover_fn(first_seg(lv), new_segtype, new_image_count);

	/* Exit without doing activation checks if the combination isn't possible */
	if (_takeover_not_possible(takeover_fn))
		return takeover_fn(lv, new_segtype, yes, force, new_image_count, new_stripes, stripe_size, allocate_pvs);

	log_verbose("Converting %s from %s to %s.",
		    display_lvname(lv), lvseg_name(first_seg(lv)),
		    (segtype_is_striped(new_segtype) && !segtype_is_any_raid0(new_segtype) &&
		    (new_stripes == 1)) ? SEG_TYPE_NAME_LINEAR : new_segtype->name);

	/* FIXME If not active, prompt and activate */
	/* FIXME Some operations do not require the LV to be active */
	/* LV must be active to perform raid conversion operations */
	if (!lv_is_active(lv)) {
		log_error("%s must be active to perform this operation.",
			  display_lvname(lv));
		return 0;
	}

	/* In clustered VGs, the LV must be active on this node exclusively. */
	if (vg_is_clustered(lv->vg) && !lv_is_active_exclusive_locally(lv)) {
		log_error("%s must be active exclusive locally to "
			  "perform this operation.", display_lvname(lv));
		return 0;
	}

	/* LV must be in sync. */
	if (!_raid_in_sync(lv)) {
		log_error("Unable to convert %s while it is not in-sync",
			  display_lvname(lv));
		return 0;
	}

	return takeover_fn(lv, new_segtype, yes, force, new_image_count, new_stripes, stripe_size, allocate_pvs);
}

static int _remove_partial_multi_segment_image(struct logical_volume *lv,
					       struct dm_list *remove_pvs)
{
	uint32_t s, extents_needed;
	struct lv_segment *rm_seg, *raid_seg = first_seg(lv);
	struct logical_volume *rm_image = NULL;
	struct physical_volume *pv;

	if (!lv_is_partial(lv))
		return_0;

	for (s = 0; s < raid_seg->area_count; s++) {
		extents_needed = 0;
		if (lv_is_partial(seg_lv(raid_seg, s)) &&
		    lv_is_on_pvs(seg_lv(raid_seg, s), remove_pvs) &&
		    (dm_list_size(&(seg_lv(raid_seg, s)->segments)) > 1)) {
			rm_image = seg_lv(raid_seg, s);

			/* First, how many damaged extents are there */
			if (lv_is_partial(seg_metalv(raid_seg, s)))
				extents_needed += seg_metalv(raid_seg, s)->le_count;
			dm_list_iterate_items(rm_seg, &rm_image->segments) {
				/*
				 * segment areas are for stripe, mirror, raid,
				 * etc.  We only need to check the first area
				 * if we are dealing with RAID image LVs.
				 */
				if (seg_type(rm_seg, 0) != AREA_PV)
					continue;
				pv = seg_pv(rm_seg, 0);
				if (pv->status & MISSING_PV)
					extents_needed += rm_seg->len;
			}
			log_debug("%u extents needed to repair %s",
				  extents_needed, rm_image->name);

			/* Second, do the other PVs have the space */
			dm_list_iterate_items(rm_seg, &rm_image->segments) {
				if (seg_type(rm_seg, 0) != AREA_PV)
					continue;
				pv = seg_pv(rm_seg, 0);
				if (pv->status & MISSING_PV)
					continue;

				if ((pv->pe_count - pv->pe_alloc_count) >
				    extents_needed) {
					log_debug("%s has enough space for %s",
						  pv_dev_name(pv),
						  rm_image->name);
					goto has_enough_space;
				}
				log_debug("Not enough space on %s for %s",
					  pv_dev_name(pv), rm_image->name);
			}
		}
	}

	/*
	 * This is likely to be the normal case - single
	 * segment images.
	 */
	return_0;

has_enough_space:
	/*
	 * Now we have a multi-segment, partial image that has enough
	 * space on just one of its PVs for the entire image to be
	 * replaced.  So, we replace the image's space with an error
	 * target so that the allocator can find that space (along with
	 * the remaining free space) in order to allocate the image
	 * anew.
	 */
	if (!replace_lv_with_error_segment(rm_image))
		return_0;

	return 1;
}

/*
 * lv_raid_replace
 * @lv
 * @remove_pvs
 * @allocate_pvs
 *
 * Replace the specified PVs.
 */
int lv_raid_replace(struct logical_volume *lv,
		    struct dm_list *remove_pvs,
		    struct dm_list *allocate_pvs)
{
	int partial_segment_removed = 0;
	uint32_t s, sd, match_count = 0;
	struct dm_list old_lvs;
	struct dm_list new_meta_lvs, new_data_lvs;
	struct lv_segment *raid_seg = first_seg(lv);
	struct lv_list *lvl;
	char *tmp_names[raid_seg->area_count * 2];

	dm_list_init(&old_lvs);
	dm_list_init(&new_meta_lvs);
	dm_list_init(&new_data_lvs);

	if (lv_is_partial(lv))
		lv->vg->cmd->partial_activation = 1;

	if (!lv_is_active_exclusive_locally(lv_lock_holder(lv))) {
		log_error("%s/%s must be active %sto perform this operation.",
			  lv->vg->name, lv->name,
			  vg_is_clustered(lv->vg) ? "exclusive locally " : "");
		return 0;
	}

	if (!mirror_in_sync() && !_raid_in_sync(lv)) {
		log_error("Unable to replace devices in %s/%s while it is"
			  " not in-sync.", lv->vg->name, lv->name);
		return 0;
	}

	if (!archive(lv->vg))
		return_0;

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

		if (lv_is_virtual(seg_lv(raid_seg, s)) ||
		    lv_is_virtual(seg_metalv(raid_seg, s)) ||
		    lv_is_on_pvs(seg_lv(raid_seg, s), remove_pvs) ||
		    lv_is_on_pvs(seg_metalv(raid_seg, s), remove_pvs))
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
			  lvseg_name(raid_seg),
			  lv->vg->name, lv->name);
		return 0;
	} else if (seg_is_raid10(raid_seg)) {
		uint32_t i, rebuilds_per_group = 0;
		/* FIXME: We only support 2-way mirrors in RAID10 currently */
		uint32_t copies = 2;

		for (i = 0; i < raid_seg->area_count * copies; i++) {
			s = i % raid_seg->area_count;
			if (!(i % copies))
				rebuilds_per_group = 0;
			if (lv_is_on_pvs(seg_lv(raid_seg, s), remove_pvs) ||
			    lv_is_on_pvs(seg_metalv(raid_seg, s), remove_pvs) ||
			    lv_is_virtual(seg_lv(raid_seg, s)) ||
			    lv_is_virtual(seg_metalv(raid_seg, s)))
				rebuilds_per_group++;
			if (rebuilds_per_group >= copies) {
				log_error("Unable to replace all the devices "
					  "in a RAID10 mirror group.");
				return 0;
			}
		}
	}

	/* Prevent any PVs holding image components from being used for allocation */
	if (!_avoid_pvs_with_other_images_of_lv(lv, allocate_pvs)) {
		log_error("Failed to prevent PVs holding image components "
			  "from being used for allocation.");
		return 0;
	}

	/*
	 * Allocate the new image components first
	 * - This makes it easy to avoid all currently used devs
	 * - We can immediately tell if there is enough space
	 *
	 * - We need to change the LV names when we insert them.
	 */
try_again:
	if (!_alloc_image_components(lv, allocate_pvs, match_count,
				     &new_meta_lvs, &new_data_lvs)) {
		if (!lv_is_partial(lv)) {
			log_error("LV %s in not partial.", display_lvname(lv));
			return 0;
		}

		/* This is a repair, so try to do better than all-or-nothing */
		match_count--;
		if (match_count > 0) {
			log_error("Failed to replace %u devices."
				  "  Attempting to replace %u instead.",
				  match_count, match_count+1);
			/*
			 * Since we are replacing some but not all of the bad
			 * devices, we must set partial_activation
			 */
			lv->vg->cmd->partial_activation = 1;
			goto try_again;
		} else if (!match_count && !partial_segment_removed) {
			/*
			 * We are down to the last straw.  We can only hope
			 * that a failed PV is just one of several PVs in
			 * the image; and if we extract the image, there may
			 * be enough room on the image's other PVs for a
			 * reallocation of the image.
			 */
			if (!_remove_partial_multi_segment_image(lv, remove_pvs))
				return_0;

			match_count = 1;
			partial_segment_removed = 1;
			lv->vg->cmd->partial_activation = 1;
			goto try_again;
		}
		log_error("Failed to allocate replacement images for %s/%s",
			  lv->vg->name, lv->name);

		return 0;
	}

	/*
	 * Remove the old images
	 * - If we did this before the allocate, we wouldn't have to rename
	 *   the allocated images, but it'd be much harder to avoid the right
	 *   PVs during allocation.
	 *
	 * - If this is a repair and we were forced to call
	 *   _remove_partial_multi_segment_image, then the remove_pvs list
	 *   is no longer relevant - _raid_extract_images is forced to replace
	 *   the image with the error target.  Thus, the full set of PVs is
	 *   supplied - knowing that only the image with the error target
	 *   will be affected.
	 */
	if (!_raid_extract_images(lv, raid_seg->area_count - match_count,
				  partial_segment_removed ?
				  &lv->vg->pvs : remove_pvs, 0,
				  &old_lvs, &old_lvs)) {
		log_error("Failed to remove the specified images from %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * Now that they are extracted and visible, make the system aware
	 * of their new names.
	 */
	dm_list_iterate_items(lvl, &old_lvs)
		if (!activate_lv_excl_local(lv->vg->cmd, lvl->lv))
			return_0;

	/*
	 * Skip metadata operation normally done to clear the metadata sub-LVs.
	 *
	 * The LV_REBUILD flag is set on the new sub-LVs,
	 * so they will be rebuilt and we don't need to clear the metadata dev.
	 */

	for (s = 0; s < raid_seg->area_count; s++) {
		sd = s + raid_seg->area_count;

		if ((seg_type(raid_seg, s) == AREA_UNASSIGNED) &&
		    (seg_metatype(raid_seg, s) == AREA_UNASSIGNED)) {
			/* Adjust the new metadata LV name */
			lvl = dm_list_item(dm_list_first(&new_meta_lvs),
					   struct lv_list);
			dm_list_del(&lvl->list);
			if (!(tmp_names[s] = _generate_raid_name(lv, "rmeta", s)))
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
			/* coverity[copy_paste_error] intentional */
			if (!(tmp_names[sd] = _generate_raid_name(lv, "rimage", s)))
				return_0;
			if (!set_lv_segment_area_lv(raid_seg, s, lvl->lv, 0,
						    lvl->lv->status)) {
				log_error("Failed to add %s to %s",
					  lvl->lv->name, lv->name);
				return 0;
			}
			lv_set_hidden(lvl->lv);
		} else
			tmp_names[s] = tmp_names[sd] = NULL;
	}

	if (!lv_update_and_reload_origin(lv))
		return_0;

	dm_list_iterate_items(lvl, &old_lvs) {
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

	if (!lv_update_and_reload_origin(lv))
		return_0;

	return 1;
}

int lv_raid_remove_missing(struct logical_volume *lv)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);

	if (!lv_is_partial(lv)) {
		log_error(INTERNAL_ERROR "%s/%s is not a partial LV",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!archive(lv->vg))
		return_0;

	log_debug("Attempting to remove missing devices from %s LV, %s",
		  lvseg_name(seg), lv->name);

	/*
	 * FIXME: Make sure # of compromised components will not affect RAID
	 */

	for (s = 0; s < seg->area_count; s++) {
		if (!lv_is_partial(seg_lv(seg, s)) &&
		    (!seg->meta_areas || !seg_metalv(seg, s) || !lv_is_partial(seg_metalv(seg, s))))
			continue;

		log_debug("Replacing %s and %s segments with error target",
			  seg_lv(seg, s)->name, seg_metalv(seg, s)->name);
		if (!replace_lv_with_error_segment(seg_lv(seg, s))) {
			log_error("Failed to replace %s's extents with error target.",
				  display_lvname(seg_lv(seg, s)));
			return 0;
		}
		if (seg->meta_areas && !replace_lv_with_error_segment(seg_metalv(seg, s))) {
			log_error("Failed to replace %s's extents with error target.",
				  display_lvname(seg_metalv(seg, s)));
			return 0;
		}
	}

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

/* Return 1 if a partial raid LV can be activated redundantly */
static int _partial_raid_lv_is_redundant(const struct logical_volume *lv)
{
	struct lv_segment *raid_seg = first_seg(lv);
	uint32_t copies;
	uint32_t i, s, rebuilds_per_group = 0;
	uint32_t failed_components = 0;

	if (seg_is_raid10(raid_seg)) {
		/* FIXME: We only support 2-way mirrors in RAID10 currently */
		copies = 2;
		for (i = 0; i < raid_seg->area_count * copies; i++) {
			s = i % raid_seg->area_count;

			if (!(i % copies))
				rebuilds_per_group = 0;

			if (lv_is_partial(seg_lv(raid_seg, s)) ||
			    lv_is_partial(seg_metalv(raid_seg, s)) ||
			    lv_is_virtual(seg_lv(raid_seg, s)) ||
			    lv_is_virtual(seg_metalv(raid_seg, s)))
				rebuilds_per_group++;

			if (rebuilds_per_group >= copies) {
				log_verbose("An entire mirror group has failed in %s.",
					    display_lvname(lv));
				return 0;	/* Insufficient redundancy to activate */
			}
		}

		return 1; /* Redundant */
	}

	for (s = 0; s < raid_seg->area_count; s++) {
		if (lv_is_partial(seg_lv(raid_seg, s)) ||
		    lv_is_partial(seg_metalv(raid_seg, s)) ||
		    lv_is_virtual(seg_lv(raid_seg, s)) ||
		    lv_is_virtual(seg_metalv(raid_seg, s)))
			failed_components++;
	}

	if (failed_components == raid_seg->area_count) {
		log_verbose("All components of raid LV %s have failed.",
			    display_lvname(lv));
		return 0;	/* Insufficient redundancy to activate */
	} else if (raid_seg->segtype->parity_devs &&
		   (failed_components > raid_seg->segtype->parity_devs)) {
		log_verbose("More than %u components from %s %s have failed.",
			    raid_seg->segtype->parity_devs,
			    lvseg_name(raid_seg),
			    display_lvname(lv));
		return 0;	/* Insufficient redundancy to activate */
	}

	return 1;
}

/* Sets *data to 1 if the LV cannot be activated without data loss */
static int _lv_may_be_activated_in_degraded_mode(struct logical_volume *lv, void *data)
{
	int *not_capable = (int *)data;
	uint32_t s;
	struct lv_segment *seg;

	if (*not_capable)
		return 1;	/* No further checks needed */

	if (!lv_is_partial(lv))
		return 1;

	if (lv_is_raid(lv)) {
		*not_capable = !_partial_raid_lv_is_redundant(lv);
		return 1;
	}

	/* Ignore RAID sub-LVs. */
	if (lv_is_raid_type(lv))
		return 1;

	dm_list_iterate_items(seg, &lv->segments)
		for (s = 0; s < seg->area_count; s++)
			if (seg_type(seg, s) != AREA_LV) {
				log_verbose("%s contains a segment incapable of degraded activation",
					    display_lvname(lv));
				*not_capable = 1;
			}

	return 1;
}

int partial_raid_lv_supports_degraded_activation(const struct logical_volume *clv)
{
	int not_capable = 0;
	struct logical_volume * lv = (struct logical_volume *)clv; /* drop const */

	if (!_lv_may_be_activated_in_degraded_mode(lv, &not_capable) || not_capable)
		return_0;

	if (!for_each_sub_lv(lv, _lv_may_be_activated_in_degraded_mode, &not_capable)) {
		log_error(INTERNAL_ERROR "for_each_sub_lv failure.");
		return 0;
	}

	return !not_capable;
}
