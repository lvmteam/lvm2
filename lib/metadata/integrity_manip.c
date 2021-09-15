/*
 * Copyright (C) 2014-2015 Red Hat, Inc. All rights reserved.
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

#include "lib/misc/lib.h"
#include "lib/metadata/metadata.h"
#include "lib/locking/locking.h"
#include "lib/misc/lvm-string.h"
#include "lib/commands/toolcontext.h"
#include "lib/display/display.h"
#include "lib/metadata/segtype.h"
#include "lib/activate/activate.h"
#include "lib/config/defaults.h"

#define DEFAULT_TAG_SIZE 4 /* bytes */
#define DEFAULT_MODE 'J'
#define DEFAULT_INTERNAL_HASH "crc32c"
#define DEFAULT_BLOCK_SIZE 512

#define ONE_MB_IN_BYTES 1048576
#define ONE_GB_IN_BYTES 1073741824

int lv_is_integrity_origin(const struct logical_volume *lv)
{
	struct seg_list *sl;

	dm_list_iterate_items(sl, &lv->segs_using_this_lv) {
		if (!sl->seg || !sl->seg->lv || !sl->seg->origin)
			continue;
		if (lv_is_integrity(sl->seg->lv) && (sl->seg->origin == lv))
			return 1;
	}
	return 0;
}

/*
 * Every 500M of data needs 4M of metadata.
 * (From trial and error testing.)
 *
 * plus some initial space for journals.
 * (again from trial and error testing.)
 */
static uint64_t _lv_size_bytes_to_integrity_meta_bytes(uint64_t lv_size_bytes)
{
	uint64_t meta_bytes;
	uint64_t initial_bytes;

	/* Every 500M of data needs 4M of metadata. */
	meta_bytes = ((lv_size_bytes / (500 * ONE_MB_IN_BYTES)) + 1) * (4 * ONE_MB_IN_BYTES);

	/*
	 * initial space used for journals
	 * lv_size <= 512M -> 4M
	 * lv_size <= 1G   -> 8M
	 * lv_size <= 4G   -> 32M
	 * lv_size > 4G    -> 64M
	 */
	if (lv_size_bytes <= (512 * ONE_MB_IN_BYTES))
		initial_bytes = 4 * ONE_MB_IN_BYTES;
	else if (lv_size_bytes <= ONE_GB_IN_BYTES)
		initial_bytes = 8 * ONE_MB_IN_BYTES;
	else if (lv_size_bytes <= (4ULL * ONE_GB_IN_BYTES))
		initial_bytes = 32 * ONE_MB_IN_BYTES;
	else if (lv_size_bytes > (4ULL * ONE_GB_IN_BYTES))
		initial_bytes = 64 * ONE_MB_IN_BYTES;

	return meta_bytes + initial_bytes;
}

/*
 * The user wants external metadata, but did not specify an existing
 * LV to hold metadata, so create an LV for metadata.
 */
static int _lv_create_integrity_metadata(struct cmd_context *cmd,
				struct volume_group *vg,
				struct lvcreate_params *lp,
				struct logical_volume **meta_lv)
{
	char metaname[NAME_LEN] = { 0 };
	uint64_t lv_size_bytes, meta_bytes, meta_sectors;
	struct logical_volume *lv;
	struct lvcreate_params lp_meta = {
		.activate = CHANGE_AN,
		.alloc = ALLOC_INHERIT,
		.major = -1,
		.minor = -1,
		.permission = LVM_READ | LVM_WRITE,
		.pvh = &vg->pvs,
		.read_ahead = DM_READ_AHEAD_NONE,
		.stripes = 1,
		.vg_name = vg->name,
		.zero = 0,
		.wipe_signatures = 0,
		.suppress_zero_warn = 1,
	};

	if (lp->lv_name &&
	    dm_snprintf(metaname, NAME_LEN, "%s_imeta", lp->lv_name) < 0) {
		log_error("Failed to create metadata LV name.");
		return 0;
	}

	lp_meta.lv_name = metaname;
	lp_meta.pvh = lp->pvh;

	lv_size_bytes = (uint64_t)lp->extents * (uint64_t)vg->extent_size * 512;
	meta_bytes = _lv_size_bytes_to_integrity_meta_bytes(lv_size_bytes);
	meta_sectors = meta_bytes / 512;
	lp_meta.extents = meta_sectors / vg->extent_size;

	log_print_unless_silent("Creating integrity metadata LV %s with size %s.",
		  metaname, display_size(cmd, meta_sectors));

	dm_list_init(&lp_meta.tags);

	if (!(lp_meta.segtype = get_segtype_from_string(vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	if (!(lv = lv_create_single(vg, &lp_meta))) {
		log_error("Failed to create integrity metadata LV");
		return 0;
	}

	if (dm_list_size(&lv->segments) > 1) {
		log_error("Integrity metadata uses more than one segment.");
		return 0;
	}

	*meta_lv = lv;
	return 1;
}

int lv_extend_integrity_in_raid(struct logical_volume *lv, struct dm_list *pvh)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	const struct segment_type *segtype;
	struct lv_segment *seg_top, *seg_image;
	struct logical_volume *lv_image;
	struct logical_volume *lv_iorig;
	struct logical_volume *lv_imeta;
	struct dm_list allocatable_pvs;
	struct dm_list *use_pvh;
	uint64_t lv_size_bytes, meta_bytes, meta_sectors, prev_meta_sectors;
	uint32_t meta_extents, prev_meta_extents;
	uint32_t area_count, s;

	if (!lv_is_raid(lv))
		return_0;

	seg_top = first_seg(lv);
		                
	if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	area_count = seg_top->area_count;

	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);
		seg_image = first_seg(lv_image);

		if (!(lv_imeta = seg_image->integrity_meta_dev)) {
			log_error("LV %s segment has no integrity metadata device.", display_lvname(lv));
			return 0;
		}

		if (!(lv_iorig = seg_lv(seg_image, 0))) {
			log_error("LV %s integrity segment has no origin", display_lvname(lv));
			return 0;
		}

		lv_size_bytes = lv_iorig->size * 512;
		meta_bytes = _lv_size_bytes_to_integrity_meta_bytes(lv_size_bytes);
		meta_sectors = meta_bytes / 512;
		meta_extents = meta_sectors / vg->extent_size;

		prev_meta_sectors = lv_imeta->size;
		prev_meta_extents = prev_meta_sectors / vg->extent_size;

		if (meta_extents <= prev_meta_extents) {
			log_debug("extend not needed for imeta LV %s", lv_imeta->name);
			continue;
		}

		/*
		 * We only allow lv_imeta to exist on a single PV (for now),
		 * so the allocatable_pvs is the one PV currently used by
		 * lv_imeta.
		 */
		dm_list_init(&allocatable_pvs);

		if (!get_pv_list_for_lv(cmd->mem, lv_imeta, &allocatable_pvs)) {
			log_error("Failed to build list of PVs for extending %s.", display_lvname(lv_imeta));
			return 0;
		}

		use_pvh = &allocatable_pvs;

		if (!lv_extend(lv_imeta, segtype, 1, 0, 0, 0,
			       meta_extents - prev_meta_extents,
			       use_pvh, lv_imeta->alloc, 0)) {
			log_error("Failed to extend integrity metadata LV %s", lv_imeta->name);
			return 0;
		}
	}

	return 1;
}

int lv_remove_integrity_from_raid(struct logical_volume *lv)
{
	struct logical_volume *iorig_lvs[DEFAULT_RAID_MAX_IMAGES];
	struct logical_volume *imeta_lvs[DEFAULT_RAID_MAX_IMAGES];
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct lv_segment *seg_top, *seg_image;
	struct logical_volume *lv_image;
	struct logical_volume *lv_iorig;
	struct logical_volume *lv_imeta;
	uint32_t area_count, s;
	int is_active = lv_is_active(lv);

	seg_top = first_seg(lv);

	if (!seg_is_raid1(seg_top) && !seg_is_raid4(seg_top) &&
	    !seg_is_any_raid5(seg_top) && !seg_is_any_raid6(seg_top) &&
	    !seg_is_any_raid10(seg_top)) {
		log_error("LV %s segment is unsupported raid for integrity.", display_lvname(lv));
		return 0;
	}

	area_count = seg_top->area_count;

	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);
		seg_image = first_seg(lv_image);

		if (!(lv_imeta = seg_image->integrity_meta_dev)) {
			log_error("LV %s segment has no integrity metadata device.", display_lvname(lv));
			return 0;
		}

		if (!(lv_iorig = seg_lv(seg_image, 0))) {
			log_error("LV %s integrity segment has no origin", display_lvname(lv));
			return 0;
		}

		if (!remove_seg_from_segs_using_this_lv(seg_image->integrity_meta_dev, seg_image))
			return_0;

		iorig_lvs[s] = lv_iorig;
		imeta_lvs[s] = lv_imeta;

		lv_image->status &= ~INTEGRITY;
		seg_image->integrity_meta_dev = NULL;
		seg_image->integrity_data_sectors = 0;
		memset(&seg_image->integrity_settings, 0, sizeof(seg_image->integrity_settings));

		if (!remove_layer_from_lv(lv_image, lv_iorig))
			return_0;
	}

	if (is_active) {
		/* vg_write(), suspend_lv(), vg_commit(), resume_lv() */
		if (!lv_update_and_reload(lv)) {
			log_error("Failed to update and reload LV after integrity remove.");
			return 0;
                }
	}

	for (s = 0; s < area_count; s++) {
		lv_iorig = iorig_lvs[s];
		lv_imeta = imeta_lvs[s];

		if (is_active) {
			if (!deactivate_lv(cmd, lv_iorig))
				log_error("Failed to deactivate unused iorig LV %s.", lv_iorig->name);

			if (!deactivate_lv(cmd, lv_imeta))
				log_error("Failed to deactivate unused imeta LV %s.", lv_imeta->name);
		}

		lv_imeta->status &= ~INTEGRITY_METADATA;
		lv_set_visible(lv_imeta);

		if (!lv_remove(lv_iorig))
			log_error("Failed to remove unused iorig LV %s.", lv_iorig->name);

		if (!lv_remove(lv_imeta))
			log_error("Failed to remove unused imeta LV %s.", lv_imeta->name);
	}

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	return 1;
}

int integrity_mode_set(const char *mode, struct integrity_settings *settings)
{
	if (!mode)
		settings->mode[0] = DEFAULT_MODE;
	else if (!strcmp(mode, "bitmap") || !strcmp(mode, "B"))
		settings->mode[0] = 'B';
	else if (!strcmp(mode, "journal") || !strcmp(mode, "J"))
		settings->mode[0] = 'J';
	else {
		log_error("Invalid raid integrity mode (use \"bitmap\" or \"journal\")");
		return 0;
	}
	return 1;
}

static int _set_integrity_block_size(struct cmd_context *cmd, struct logical_volume *lv, int is_active,
				     struct integrity_settings *settings,
				     int lbs_4k, int lbs_512, int pbs_4k, int pbs_512)
{
	char pathname[PATH_MAX];
	uint32_t fs_block_size = 0;
	int rv;

	if (lbs_4k && lbs_512) {
		log_error("Integrity requires consistent logical block size for LV devices.");
		goto bad;
	}

	if (settings->block_size &&
	    (settings->block_size != 512 && settings->block_size != 1024 &&
	     settings->block_size != 2048 && settings->block_size != 4096)) {
		log_error("Invalid integrity block size, possible values are 512, 1024, 2048, 4096");
		goto bad;
	}

	if (lbs_4k && settings->block_size && (settings->block_size < 4096)) {
		log_error("Integrity block size %u not allowed with device logical block size 4096.",
			  settings->block_size);
		goto bad;
	}

	if (!strcmp(cmd->name, "lvcreate")) {
		if (lbs_4k) {
			settings->block_size = 4096;
		} else if (lbs_512 && pbs_4k && !pbs_512) {
			settings->block_size = 4096;
		} else if (lbs_512) {
			if (!settings->block_size)
				settings->block_size = 512;
		} else if (!lbs_4k && !lbs_512) {
			if (!settings->block_size)
				settings->block_size = 512;
			log_print("Using integrity block size %u with unknown device logical block size.",
				  settings->block_size);
		} else {
			goto_bad;
		}

	} else if (!strcmp(cmd->name, "lvconvert")) {
		if (dm_snprintf(pathname, sizeof(pathname), "%s%s/%s", cmd->dev_dir,
				lv->vg->name, lv->name) < 0) {
			log_error("Path name too long to get LV block size %s", display_lvname(lv));
			goto bad;
		}

		/*
		 * get_fs_block_size() returns the libblkid BLOCK_SIZE value,
		 * where libblkid has fs-specific code to set BLOCK_SIZE to the
		 * value we need here.
		 *
		 * The term "block size" here may not equate directly to what the fs
		 * calls the block size, e.g. xfs calls this the sector size (and
		 * something different the block size); while ext4 does call this
		 * value the block size, but it's possible values are not the same
		 * as xfs's, and do not seem to relate directly to the device LBS.
		 */
		rv = get_fs_block_size(pathname, &fs_block_size);
		if (!rv || !fs_block_size) {
			int use_bs;

			if (lbs_4k && pbs_4k) {
				use_bs = 4096;
			} else if (lbs_512 && pbs_512) {
				use_bs = 512;
			} else if (lbs_512 && pbs_4k) {
				if (settings->block_size == 4096)
					use_bs = 4096;
				else
					use_bs = 512;
			} else {
				use_bs = 512;
			}

			if (settings->block_size && (settings->block_size != use_bs)) {
				log_error("Cannot use integrity block size %u with unknown file system block size, logical block size %u, physical block size %u.",
					   settings->block_size, lbs_4k ? 4096 : 512, pbs_4k ? 4096 : 512);
				goto bad;
			}

			settings->block_size = use_bs;

			log_print("Using integrity block size %u for unknown file system block size, logical block size %u, physical block size %u.",
				  settings->block_size, lbs_4k ? 4096 : 512, pbs_4k ? 4096 : 512);
			goto out;
		}

		if (!settings->block_size) {
			if (is_active && lbs_512) {
				/* increasing the lbs from 512 to 4k under an active LV could cause problems
				   for an application that expects a given io size/alignment is possible. */
				settings->block_size = 512;
				if (fs_block_size > 512)
					log_print("Limiting integrity block size to 512 because the LV is active.");
			} else if (fs_block_size <= 4096)
				settings->block_size = fs_block_size;
			else
				settings->block_size = 4096; /* dm-integrity max is 4096 */
			log_print("Using integrity block size %u for file system block size %u.",
				  settings->block_size, fs_block_size);
		} else {
			/* let user specify integrity block size that is less than fs block size */
			if (settings->block_size > fs_block_size) {
				log_error("Integrity block size %u cannot be larger than file system block size %u.",
					  settings->block_size, fs_block_size);
				goto bad;
			}
			log_print("Using integrity block size %u for file system block size %u.",
				  settings->block_size, fs_block_size);
		}
	}
out:
	return 1;
bad:
	return 0;
}

/*
 * Add integrity to each raid image.
 *
 * for each rimage_N:
 * . create and allocate a new linear LV rimage_N_imeta
 * . move the segments from rimage_N to a new rimage_N_iorig
 * . add an integrity segment to rimage_N with
 *   origin=rimage_N_iorig, meta_dev=rimage_N_imeta
 *
 * Before:
 * rimage_0
 *   segment1: striped: pv0:A
 * rimage_1
 *   segment1: striped: pv1:B
 *
 * After:
 * rimage_0
 *   segment1: integrity: rimage_0_iorig, rimage_0_imeta
 * rimage_1
 *   segment1: integrity: rimage_1_iorig, rimage_1_imeta
 * rimage_0_iorig
 *   segment1: striped: pv0:A
 * rimage_1_iorig
 *   segment1: striped: pv1:B
 * rimage_0_imeta
 *   segment1: striped: pv2:A
 * rimage_1_imeta
 *   segment1: striped: pv2:B
 *    
 */

int lv_add_integrity_to_raid(struct logical_volume *lv, struct integrity_settings *settings,
			     struct dm_list *pvh, struct logical_volume *lv_imeta_0)
{
	char imeta_name[NAME_LEN];
	char *imeta_name_dup;
	struct lvcreate_params lp;
	struct dm_list allocatable_pvs;
	struct logical_volume *imeta_lvs[DEFAULT_RAID_MAX_IMAGES];
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_image, *lv_imeta, *lv_iorig;
	struct lv_segment *seg_top, *seg_image;
	struct pv_list *pvl;
	const struct segment_type *segtype;
	struct integrity_settings *set = NULL;
	struct dm_list *use_pvh = NULL;
	uint32_t area_count, s;
	uint32_t revert_meta_lvs = 0;
	int lbs_4k = 0, lbs_512 = 0, lbs_unknown = 0;
	int pbs_4k = 0, pbs_512 = 0, pbs_unknown = 0;
	int is_active;

	memset(imeta_lvs, 0, sizeof(imeta_lvs));

	is_active = lv_is_active(lv);

	if (dm_list_size(&lv->segments) != 1)
		return_0;

	if (!dm_list_empty(&lv->segs_using_this_lv)) {
		log_error("Integrity can only be added to top level raid LV.");
		return 0;
	}

	if (lv_is_origin(lv)) {
		log_error("Integrity cannot be added to snapshot origins.");
		return 0;
	}

	seg_top = first_seg(lv);
	area_count = seg_top->area_count;

	if (!seg_is_raid1(seg_top) && !seg_is_raid4(seg_top) &&
	    !seg_is_any_raid5(seg_top) && !seg_is_any_raid6(seg_top) &&
	    !seg_is_any_raid10(seg_top)) {
		log_error("Integrity can only be added to raid1,4,5,6,10.");
		return 0;
	}

	/*
	 * For each rimage, create an _imeta LV for integrity metadata.
	 * Each needs to be zeroed.
	 */
	for (s = 0; s < area_count; s++) {
		struct logical_volume *meta_lv;
		struct wipe_params wipe = { .do_zero = 1 };

		if (s >= DEFAULT_RAID_MAX_IMAGES)
			goto_bad;

		lv_image = seg_lv(seg_top, s);

		/*
		 * This function is used to add integrity to new images added
		 * to the raid, in which case old images will already be
		 * integrity.
		 */
		if (seg_is_integrity(first_seg(lv_image)))
			continue;

		if (!seg_is_striped(first_seg(lv_image))) {
			log_error("raid image must be linear to add integrity");
			goto bad;
		}

		/*
		 * Use an existing lv_imeta from previous linear+integrity LV.
		 * FIXME: is it guaranteed that lv_image_0 is the existing?
		 */
		if (!s && lv_imeta_0) {
			if (dm_snprintf(imeta_name, sizeof(imeta_name), "%s_imeta", lv_image->name) > 0) {
				if ((imeta_name_dup = dm_pool_strdup(vg->vgmem, imeta_name)))
					lv_imeta_0->name = imeta_name_dup;
			}
			imeta_lvs[0] = lv_imeta_0;
			continue;
		}

		dm_list_init(&allocatable_pvs);

		if (!get_pv_list_for_lv(cmd->mem, lv_image, &allocatable_pvs)) {
			log_error("Failed to build list of PVs for %s.", display_lvname(lv_image));
			goto bad;
		}

		dm_list_iterate_items(pvl, &allocatable_pvs) {
			unsigned int pbs = 0;
			unsigned int lbs = 0;

			if (!dev_get_direct_block_sizes(pvl->pv->dev, &pbs, &lbs)) {
				lbs_unknown++;
				pbs_unknown++;
				continue;
			}
			if (lbs == 4096)
				lbs_4k++;
			else if (lbs == 512)
				lbs_512++;
			else
				lbs_unknown++;
			if (pbs == 4096)
				pbs_4k++;
			else if (pbs == 512)
				pbs_512++;
			else
				pbs_unknown++;
		}

		use_pvh = &allocatable_pvs;

		/*
		 * allocate a new linear LV NAME_rimage_N_imeta
		 */
		memset(&lp, 0, sizeof(lp));
		lp.lv_name = lv_image->name;
		lp.pvh = use_pvh;
		lp.extents = lv_image->size / vg->extent_size;

		if (!_lv_create_integrity_metadata(cmd, vg, &lp, &meta_lv))
			goto_bad;

		revert_meta_lvs++;

		/* Used below to set up the new integrity segment. */
		imeta_lvs[s] = meta_lv;

		/*
		 * dm-integrity requires the metadata LV header to be zeroed.
		 */

		if (!activate_lv(cmd, meta_lv)) {
			log_error("Failed to activate LV %s to zero", display_lvname(meta_lv));
			goto bad;
		}

		if (!wipe_lv(meta_lv, wipe)) {
			log_error("Failed to zero LV for integrity metadata %s", display_lvname(meta_lv));
			if (deactivate_lv(cmd, meta_lv))
				log_error("Failed to deactivate LV %s after zero", display_lvname(meta_lv));
			goto bad;
		}

		if (!deactivate_lv(cmd, meta_lv)) {
			log_error("Failed to deactivate LV %s after zero", display_lvname(meta_lv));
			goto bad;
		}
	}

	if (!is_active) {
		/* checking block size of fs on the lv requires the lv to be active */
		if (!activate_lv(cmd, lv)) {
			log_error("Failed to activate LV to check block size %s", display_lvname(lv));
			goto bad;
		}
		if (!sync_local_dev_names(cmd))
			stack;
	}

	/*
	 * Set settings->block_size which will be copied to segment settings below.
	 * integrity block size chosen based on device logical block size and
	 * file system block size.
	 */
	if (!_set_integrity_block_size(cmd, lv, is_active, settings, lbs_4k, lbs_512, pbs_4k, pbs_512)) {
		if (!is_active && !deactivate_lv(cmd, lv))
			stack;
		goto_bad;
	}

	if (!is_active) {
		if (!deactivate_lv(cmd, lv)) {
			log_error("Failed to deactivate LV after checking block size %s", display_lvname(lv));
			goto bad;
		}
	}

	/*
	 * For each rimage, move its segments to a new rimage_iorig and give
	 * the rimage a new integrity segment.
	 */
	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);

		/* Not adding integrity to this image. */
		if (!imeta_lvs[s])
			continue;

		if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_INTEGRITY)))
			goto_bad;

		log_debug("Adding integrity to raid image %s", lv_image->name);

		/*
		 * "lv_iorig" is a new LV with new id, but with the segments
		 * from "lv_image". "lv_image" keeps the existing name and id,
		 * but gets a new integrity segment, in place of the segments
		 * that were moved to lv_iorig.
		 */
		if (!(lv_iorig = insert_layer_for_lv(cmd, lv_image, INTEGRITY, "_iorig")))
			goto_bad;

		lv_image->status |= INTEGRITY;

		/*
		 * Set up the new first segment of lv_image as integrity.
		 */
		seg_image = first_seg(lv_image);
		seg_image->segtype = segtype;

		lv_imeta = imeta_lvs[s];
		lv_imeta->status |= INTEGRITY_METADATA;
		lv_set_hidden(lv_imeta);
		seg_image->integrity_data_sectors = lv_image->size;
		seg_image->integrity_meta_dev = lv_imeta;
		seg_image->integrity_recalculate = 1;

		memcpy(&seg_image->integrity_settings, settings, sizeof(struct integrity_settings));
		set = &seg_image->integrity_settings;

		if (!set->mode[0])
			set->mode[0] = DEFAULT_MODE;

		if (!set->tag_size)
			set->tag_size = DEFAULT_TAG_SIZE;

		if (!set->block_size)
			set->block_size = DEFAULT_BLOCK_SIZE;

		if (!set->internal_hash)
			set->internal_hash = DEFAULT_INTERNAL_HASH;
	}

	if (is_active) {
		log_debug("Writing VG and updating LV with new integrity LV %s", lv->name);

		/* vg_write(), suspend_lv(), vg_commit(), resume_lv() */
		if (!lv_update_and_reload(lv)) {
			log_error("LV update and reload failed");
			goto bad;
		}
		revert_meta_lvs = 0;

	} else {
		log_debug("Writing VG with new integrity LV %s", lv->name);

		if (!vg_write(vg) || !vg_commit(vg))
			goto_bad;

		revert_meta_lvs = 0;

		/*
		 * This first activation includes "recalculate" which starts the
		 * kernel's recalculating (initialization) process.
		 */

		log_debug("Activating to start integrity initialization for LV %s", lv->name);

		if (!activate_lv(cmd, lv)) {
			log_error("Failed to activate integrity LV to initialize.");
			goto bad;
		}
	}

	/*
	 * Now that the device is being initialized, update the VG to clear
	 * integrity_recalculate so that subsequent activations will not
	 * include "recalculate" and restart initialization.
	 */

	log_debug("Writing VG with initialized integrity LV %s", lv->name);

	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);
		seg_image = first_seg(lv_image);
		seg_image->integrity_recalculate = 0;
	}

	if (!vg_write(vg) || !vg_commit(vg))
		goto_bad;

	return 1;

bad:
	log_error("Failed to add integrity.");

	if (revert_meta_lvs) {
		for (s = 0; s < DEFAULT_RAID_MAX_IMAGES; s++) {
			if (!imeta_lvs[s])
				continue;
			if (!lv_remove(imeta_lvs[s]))
				log_error("New integrity metadata LV may require manual removal.");
		}
	}
			       
	if (!vg_write(vg) || !vg_commit(vg))
		log_error("New integrity metadata LV may require manual removal.");

	return 0;
}

/*
 * This should rarely if ever be used.  A command that adds integrity
 * to an LV will activate and then clear the flag.  If it fails before
 * clearing the flag, then this function will be used by a subsequent
 * activation to clear the flag.
 */
void lv_clear_integrity_recalculate_metadata(struct logical_volume *lv)
{
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;

	if (!lv_is_raid(lv) && !lv_is_integrity(lv)) {
		log_error("Invalid LV type for clearing integrity");
		return;
	}

	seg = first_seg(lv);

	if (seg_is_raid(seg)) {
		for (s = 0; s < seg->area_count; s++) {
			lv_image = seg_lv(seg, s);
			seg_image = first_seg(lv_image);
			seg_image->integrity_recalculate = 0;
		}
	} else if (seg_is_integrity(seg)) {
		seg->integrity_recalculate = 0;
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		log_warn("WARNING: failed to clear integrity recalculate flag for %s",
			 display_lvname(lv));
	}
}

int lv_has_integrity_recalculate_metadata(struct logical_volume *lv)
{
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;
	int ret = 0;

	if (!lv_is_raid(lv) && !lv_is_integrity(lv))
		return 0;

	seg = first_seg(lv);

	if (seg_is_raid(seg)) {
		for (s = 0; s < seg->area_count; s++) {
			lv_image = seg_lv(seg, s);
			seg_image = first_seg(lv_image);

			if (!seg_is_integrity(seg_image))
				continue;
			if (seg_image->integrity_recalculate)
				ret = 1;
		}
	} else if (seg_is_integrity(seg)) {
		ret = seg->integrity_recalculate;
	}

	return ret;
}

int lv_raid_has_integrity(struct logical_volume *lv)
{
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;

	if (!lv_is_raid(lv))
		return 0;

	seg = first_seg(lv);

	for (s = 0; s < seg->area_count; s++) {
		lv_image = seg_lv(seg, s);
		seg_image = first_seg(lv_image);

		if (seg_is_integrity(seg_image))
			return 1;
	}

	return 0;
}

int lv_get_raid_integrity_settings(struct logical_volume *lv, struct integrity_settings **isettings)
{
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;

	if (!lv_is_raid(lv))
		return_0;

	seg = first_seg(lv);

	for (s = 0; s < seg->area_count; s++) {
		lv_image = seg_lv(seg, s);
		seg_image = first_seg(lv_image);

		if (seg_is_integrity(seg_image)) {
			*isettings = &seg_image->integrity_settings;
			return 1;
		}
	}

	return 0;
}

int lv_raid_integrity_total_mismatches(struct cmd_context *cmd,
			    const struct logical_volume *lv,
			    uint64_t *mismatches)
{
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;
	uint64_t mismatches_image;
	uint64_t total = 0;
	int errors = 0;

	if (!lv_is_raid(lv))
		return 0;

	seg = first_seg(lv);

	for (s = 0; s < seg->area_count; s++) {
		lv_image = seg_lv(seg, s);
		seg_image = first_seg(lv_image);

		if (!seg_is_integrity(seg_image))
			continue;

		mismatches_image = 0;

		if (!lv_integrity_mismatches(cmd, lv_image, &mismatches_image))
			errors++;

		total += mismatches_image;
	}
	*mismatches = total;

	if (errors)
		return 0;
	return 1;
}

int lv_integrity_mismatches(struct cmd_context *cmd,
			    const struct logical_volume *lv,
			    uint64_t *mismatches)
{
	struct lv_with_info_and_seg_status status;

	if (lv_is_raid(lv) && lv_raid_has_integrity((struct logical_volume *)lv))
		return lv_raid_integrity_total_mismatches(cmd, lv, mismatches);

	if (!lv_is_integrity(lv))
		return_0;

	memset(&status, 0, sizeof(status));
	status.seg_status.type = SEG_STATUS_NONE;

	status.seg_status.seg = first_seg(lv);

	/* FIXME: why reporter_pool? */
	if (!(status.seg_status.mem = dm_pool_create("reporter_pool", 1024))) {
		log_error("Failed to get mem for LV status.");
		return 0;
	}

	if (!lv_info_with_seg_status(cmd, first_seg(lv), &status, 1, 1)) {
		log_error("Failed to get device mapper status for %s", display_lvname(lv));
		goto fail;
	}

	if (!status.info.exists)
		goto fail;

	if (status.seg_status.type != SEG_STATUS_INTEGRITY) {
		log_error("Invalid device mapper status type (%d) for %s",
			  (uint32_t)status.seg_status.type, display_lvname(lv));
		goto fail;
	}

	*mismatches = status.seg_status.integrity->number_of_mismatches;

	dm_pool_destroy(status.seg_status.mem);
	return 1;

fail:
	dm_pool_destroy(status.seg_status.mem);
	return 0;
}

