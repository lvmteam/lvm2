/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2016 Red Hat, Inc. All rights reserved.
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
#include <defaults.h>
#include "metadata.h"
#include "lv_alloc.h"
#include "pv_alloc.h"
#include "str_list.h"
#include "segtype.h"

/*
 * Attempt to merge two adjacent segments.
 * Currently only supports striped segments on AREA_PV.
 * Returns success if successful, in which case 'first'
 * gets adjusted to contain both areas.
 */
static int _merge(struct lv_segment *first, struct lv_segment *second)
{
	if (!first || !second || first->segtype != second->segtype ||
	    !first->segtype->ops->merge_segments)
		return 0;

	return first->segtype->ops->merge_segments(first, second);
}

int lv_merge_segments(struct logical_volume *lv)
{
	struct dm_list *segh, *t;
	struct lv_segment *seg, *current, *prev = NULL;

	/*
	 * Don't interfere with pvmoves as they rely upon two LVs
	 * having a matching segment structure.
	 */

	if (lv_is_locked(lv) || lv_is_pvmove(lv))
		return 1;

	if (lv_is_mirror_image(lv) &&
	    (seg = get_only_segment_using_this_lv(lv)) &&
	    (lv_is_locked(seg->lv) || lv_is_pvmove(seg->lv)))
		return 1;

	dm_list_iterate_safe(segh, t, &lv->segments) {
		current = dm_list_item(segh, struct lv_segment);

		if (_merge(prev, current))
			dm_list_del(&current->list);
		else
			prev = current;
	}

	return 1;
}

#define ERROR_MAX 100
#define inc_error_count \
	if (error_count++ > ERROR_MAX)	\
		goto out

/*
 * RAID segment property checks.
 *
 * Checks in here shall catch any
 * bogus segment structure setup.
 */
#define raid_seg_error(msg) { \
	log_error("LV %s invalid: %s for %s segment", \
		  seg->lv->name, (msg), lvseg_name(seg)); \
	if ((*error_count)++ > ERROR_MAX) \
		return; \
}

#define raid_seg_error_val(msg, val) { \
	log_error("LV %s invalid: %s (is %u) for %s segment", \
		  seg->lv->name, (msg), (val), lvseg_name(seg)); \
	if ((*error_count)++ > ERROR_MAX) \
		return; \
}

/* Check raid0 segment properties in @seg */
static void _check_raid0_seg(struct lv_segment *seg, int *error_count)
{
	if (seg_is_raid0_meta(seg) &&
	    !seg->meta_areas)
		raid_seg_error("no meta areas");
	if (!seg_is_raid0_meta(seg) &&
	    seg->meta_areas)
		raid_seg_error("meta areas");
	if (!seg->stripe_size)
		raid_seg_error("zero stripe size");
	if (!is_power_of_2(seg->stripe_size))
		raid_seg_error_val("non power of 2 stripe size", seg->stripe_size);
	if (seg->region_size)
		raid_seg_error_val("non-zero region_size", seg->region_size);
	if (seg->writebehind)
		raid_seg_error_val("non-zero write behind", seg->writebehind);
	if (seg->min_recovery_rate)
		raid_seg_error_val("non-zero min recovery rate", seg->min_recovery_rate);
	if (seg->max_recovery_rate)
		raid_seg_error_val("non-zero max recovery rate", seg->max_recovery_rate);
}

/* Check RAID @seg for non-zero, power of 2 region size and min recovery rate <= max */
static void _check_raid_region_recovery(struct lv_segment *seg, int *error_count)
{
	if (!seg->region_size)
		raid_seg_error("zero region_size");
	if (!is_power_of_2(seg->region_size))
		raid_seg_error_val("non power of 2 region size", seg->region_size);
	/* min/max recovery rate may be zero but min may not be larger than max if set */
	if (seg->max_recovery_rate &&
	    seg->min_recovery_rate > seg->max_recovery_rate)
		raid_seg_error_val("min recovery larger than max recovery", seg->min_recovery_rate);
}

/* Check raid1 segment properties in @seg */
static void _check_raid1_seg(struct lv_segment *seg, int *error_count)
{
	if (!seg->meta_areas)
		raid_seg_error("no meta areas");
	if (seg->stripe_size)
		raid_seg_error_val("non-zero stripe size", seg->stripe_size);
	_check_raid_region_recovery(seg, error_count);
}

/* Check raid4/5/6/10 segment properties in @seg */
static void _check_raid45610_seg(struct lv_segment *seg, int *error_count)
{
	/* Checks applying to any raid4/5/6/10 */
	if (!seg->meta_areas)
		raid_seg_error("no meta areas");
	if (!seg->stripe_size)
		raid_seg_error("zero stripe size");
	if (!is_power_of_2(seg->stripe_size))
		raid_seg_error_val("non power of 2 stripe size", seg->stripe_size);
	_check_raid_region_recovery(seg, error_count);
	/* END: checks applying to any raid4/5/6/10 */

	/* Specific checks per raid level */
	if (seg_is_raid4(seg) ||
	    seg_is_any_raid5(seg)) {
		/*
		 * To allow for takeover between the MD raid1 and
		 * raid4/5 personalities, exactly 2 areas (i.e. DataLVs)
		 * can be mirrored by all raid1, raid4 and raid5 personalities.
		 * Hence allow a minimum of 2 areas.
		 */
		if (seg->area_count < 2)
			raid_seg_error_val("minimum 2 areas required", seg->area_count);
	} else if (seg_is_any_raid6(seg)) {
		/*
		 * FIXME: MD raid6 supports a minimum of 4 areas.
		 *	  LVM requests a minimum of 5 due to easier
		 *	  processing of SubLVs to replace.
		 *
		 *	  Once that obstacle got removed, allow for a minimum of 4.
		 */
		if (seg->area_count < 5)
			raid_seg_error_val("minimum 5 areas required", seg->area_count);
	} else if (seg_is_raid10(seg)) {
		/*
		 * FIXME: raid10 area_count minimum has to change to 2 once we
		 *	  support data_copies and odd numbers of stripes
		 */
		if (seg->area_count < 4)
			raid_seg_error_val("minimum 4 areas required", seg->area_count);
		if (seg->writebehind)
			raid_seg_error_val("non-zero writebehind", seg->writebehind);
	}
}

/* Check any non-RAID segment struct members in @seg and increment @error_count for any bogus ones */
static void _check_non_raid_seg_members(struct lv_segment *seg, int *error_count)
{
	if (seg->origin) /* snap and thin */
		raid_seg_error("non-zero origin LV");
	if (seg->indirect_origin) /* thin */
		raid_seg_error("non-zero indirect_origin LV");
	if (seg->merge_lv) /* thin */
		raid_seg_error("non-zero merge LV");
	if (seg->cow) /* snap */
		raid_seg_error("non-zero cow LV");
	if (!dm_list_empty(&seg->origin_list)) /* snap */
		raid_seg_error("non-zero origin_list");
	if (seg->log_lv)
		raid_seg_error("non-zero log LV");
	if (seg->segtype_private)
		raid_seg_error("non-zero segtype_private");
	/* thin members */
	if (seg->metadata_lv)
		raid_seg_error("non-zero metadata LV");
	if (seg->transaction_id)
		raid_seg_error("non-zero transaction_id");
	if (seg->zero_new_blocks)
		raid_seg_error("non-zero zero_new_blocks");
	if (seg->discards)
		raid_seg_error("non-zero discards");
	if (!dm_list_empty(&seg->thin_messages))
		raid_seg_error("non-zero thin_messages list");
	if (seg->external_lv)
		raid_seg_error("non-zero external LV");
	if (seg->pool_lv)
		raid_seg_error("non-zero pool LV");
	if (seg->device_id)
		raid_seg_error("non-zero device_id");
	/* cache members */
	if (seg->cache_mode)
		raid_seg_error("non-zero cache_mode");
	if (seg->policy_name)
		raid_seg_error("non-zero policy_name");
	if (seg->policy_settings)
		raid_seg_error("non-zero policy_settings");
	if (seg->cleaner_policy)
		raid_seg_error("non-zero cleaner_policy");
	/* replicator members (deprecated) */
	if (seg->replicator)
		raid_seg_error("non-zero replicator");
	if (seg->rlog_lv)
		raid_seg_error("non-zero rlog LV");
	if (seg->rlog_type)
		raid_seg_error("non-zero rlog type");
	if (seg->rdevice_index_highest)
		raid_seg_error("non-zero rdevice_index_highests");
	if (seg->rsite_index_highest)
		raid_seg_error("non-zero rsite_index_highests");
	/* .... more members? */
}

/*
 * Check RAID segment sruct members of @seg for acceptable
 * properties and increment @error_count for any bogus ones.
 */
static void _check_raid_seg(struct lv_segment *seg, int *error_count)
{
	uint32_t area_len, s;

	/* General checks applying to all RAIDs */
	if (!seg_is_raid(seg))
		raid_seg_error("erroneous RAID check");

	if (!seg->area_count)
		raid_seg_error("zero area count");

	if (!seg->areas)
		raid_seg_error("zero areas");

	if (seg->extents_copied > seg->area_len)
		raid_seg_error_val("extents_copied too large", seg->extents_copied);

	/* Default < 10, change once raid1 split shift and rename SubLVs works! */
	if (seg_is_raid1(seg)) {
		if (seg->area_count > DEFAULT_RAID1_MAX_IMAGES) {
			log_error("LV %s invalid: maximum supported areas %u (is %u) for %s segment",
			  	seg->lv->name, DEFAULT_RAID1_MAX_IMAGES, seg->area_count, lvseg_name(seg));
				if ((*error_count)++ > ERROR_MAX)
					return;
		}
	} else if (seg->area_count > DEFAULT_RAID_MAX_IMAGES) {
		log_error("LV %s invalid: maximum supported areas %u (is %u) for %s segment",
		  	seg->lv->name, DEFAULT_RAID_MAX_IMAGES, seg->area_count, lvseg_name(seg));
			if ((*error_count)++ > ERROR_MAX)
				return;
	}

	if (seg->chunk_size)
		raid_seg_error_val("non-zero chunk_size", seg->chunk_size);

	/* FIXME: should we check any non-RAID segment struct members at all? */
	_check_non_raid_seg_members(seg, error_count);

	/* Check for any DataLV flaws like non-existing ones or size variations */
	for (area_len = s = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) != AREA_LV)
			raid_seg_error("no DataLV");
		if (!lv_is_raid_image(seg_lv(seg, s)))
			raid_seg_error("DataLV without RAID image flag");
		if (area_len &&
		    area_len != seg_lv(seg, s)->le_count) {
				raid_seg_error_val("DataLV size variations",
		    				   seg_lv(seg, s)->le_count);
		} else
			area_len = seg_lv(seg, s)->le_count;
	}

	/* Check for any MetaLV flaws like non-existing ones or size variations */
	if (seg->meta_areas)
		for (area_len = s = 0; s < seg->area_count; s++) {
			if (seg_metatype(seg, s) != AREA_LV) {
				raid_seg_error("no MetaLV");
				continue;
			}
			if (!lv_is_raid_metadata(seg_metalv(seg, s)))
				raid_seg_error("MetaLV without RAID metadata flag");
			if (area_len &&
			    area_len != seg_metalv(seg, s)->le_count) {
				raid_seg_error_val("MetaLV size variations",
		    				   seg_metalv(seg, s)->le_count);
			} else
				area_len = seg_metalv(seg, s)->le_count;
		}
	/* END: general checks applying to all RAIDs */

	/* Specific segment type checks from here on */
	if (seg_is_any_raid0(seg))
		_check_raid0_seg(seg, error_count);
	else if (seg_is_raid1(seg))
		_check_raid1_seg(seg, error_count);
	else if (seg_is_raid4(seg) ||
		 seg_is_any_raid5(seg) ||
		 seg_is_any_raid6(seg) ||
		 seg_is_raid10(seg))
		_check_raid45610_seg(seg, error_count);
	else
		raid_seg_error("bogus RAID segment type");
}
/* END: RAID segment property checks. */

/*
 * Verify that an LV's segments are consecutive, complete and don't overlap.
 */
int check_lv_segments(struct logical_volume *lv, int complete_vg)
{
	struct lv_segment *seg, *seg2;
	uint32_t le = 0;
	unsigned seg_count = 0, seg_found;
	uint32_t area_multiplier, s;
	struct seg_list *sl;
	struct glv_list *glvl;
	int error_count = 0;
	struct replicator_site *rsite;
	struct replicator_device *rdev;

	/* Check LV flags match first segment type */
	if (complete_vg) {
		if (lv_is_thin_volume(lv)) {
			if (dm_list_size(&lv->segments) != 1) {
				log_error("LV %s is thin volume without exactly one segment.",
					  lv->name);
				inc_error_count;
			} else if (!seg_is_thin_volume(first_seg(lv))) {
				log_error("LV %s is thin volume without first thin volume segment.",
					  lv->name);
				inc_error_count;
			}
		}

		if (lv_is_thin_pool(lv)) {
			if (dm_list_size(&lv->segments) != 1) {
				log_error("LV %s is thin pool volume without exactly one segment.",
					  lv->name);
				inc_error_count;
			} else if (!seg_is_thin_pool(first_seg(lv))) {
				log_error("LV %s is thin pool without first thin pool segment.",
					  lv->name);
				inc_error_count;
			}
		}

		if (lv_is_pool_data(lv) &&
		    (!(seg2 = first_seg(lv)) || !(seg2 = find_pool_seg(seg2)) ||
		     seg2->area_count != 1 || seg_type(seg2, 0) != AREA_LV ||
		     seg_lv(seg2, 0) != lv)) {
			log_error("LV %s: segment 1 pool data LV does not point back to same LV",
				  lv->name);
			inc_error_count;
		}

		if (lv_is_pool_metadata(lv)) {
			if (!(seg2 = first_seg(lv)) || !(seg2 = find_pool_seg(seg2)) ||
			    seg2->metadata_lv != lv) {
				log_error("LV %s: segment 1 pool metadata LV does not point back to same LV",
					  lv->name);
				inc_error_count;
			}
			if (lv_is_thin_pool_metadata(lv) &&
			    !strstr(lv->name, "_tmeta")) {
				log_error("LV %s: thin pool metadata LV does not use _tmeta",
					  lv->name);
				inc_error_count;
			} else if (lv_is_cache_pool_metadata(lv) &&
				   !strstr(lv->name, "_cmeta")) {
				log_error("LV %s: cache pool metadata LV does not use _cmeta",
					  lv->name);
				inc_error_count;
			}
		}

		if (lv_is_external_origin(lv)) {
			seg_found = 0;
			dm_list_iterate_items(sl, &lv->segs_using_this_lv)
				if (sl->seg->external_lv == lv)
					seg_found++;
			if (seg_found != lv->external_count) {
				log_error("LV %s: external origin count does not match.",
					  lv->name);
				inc_error_count;
			}
		}
	}

	dm_list_iterate_items(seg, &lv->segments) {
		seg_count++;

		if (complete_vg && seg_is_raid(seg))
			 _check_raid_seg(seg, &error_count);
		
		if (seg->le != le) {
			log_error("LV %s invalid: segment %u should begin at "
				  "LE %" PRIu32 " (found %" PRIu32 ").",
				  lv->name, seg_count, le, seg->le);
			inc_error_count;
		}

		area_multiplier = segtype_is_striped(seg->segtype) ?
					seg->area_count : 1;

		if (seg->area_len * area_multiplier != seg->len) {
			log_error("LV %s: segment %u has inconsistent "
				  "area_len %u",
				  lv->name, seg_count, seg->area_len);
			inc_error_count;
		}

		if (lv_is_error_when_full(lv) &&
		    !seg_can_error_when_full(seg)) {
			log_error("LV %s: segment %u (%s) does not support flag "
				  "ERROR_WHEN_FULL.", lv->name, seg_count, seg->segtype->name);
			inc_error_count;
		}

		if (complete_vg && seg->log_lv &&
		    !seg_is_mirrored(seg) && lv_is_raid_image(lv)) {
			log_error("LV %s: segment %u log LV %s is not a "
				  "mirror log or a RAID image",
				  lv->name, seg_count, seg->log_lv->name);
			inc_error_count;
		}

		/*
		 * Check mirror log - which is attached to the mirrored seg
		 */
		if (complete_vg && seg->log_lv && seg_is_mirrored(seg)) {
			if (!lv_is_mirror_log(seg->log_lv)) {
				log_error("LV %s: segment %u log LV %s is not "
					  "a mirror log",
					  lv->name, seg_count, seg->log_lv->name);
				inc_error_count;
			}

			if (!(seg2 = first_seg(seg->log_lv)) ||
			    find_mirror_seg(seg2) != seg) {
				log_error("LV %s: segment %u log LV does not "
					  "point back to mirror segment",
					  lv->name, seg_count);
				inc_error_count;
			}
		}

		if (complete_vg && lv_is_mirror_image(lv)) {
			if (!(seg2 = find_mirror_seg(seg)) ||
			    !seg_is_mirrored(seg2)) {
				log_error("LV %s: segment %u mirror image "
					  "is not mirrored",
					  lv->name, seg_count);
				inc_error_count;
			}
		}

		/* Check the various thin segment types */
		if (complete_vg) {
			if (seg_is_thin_pool(seg)) {
				if (!lv_is_thin_pool(lv)) {
					log_error("LV %s is missing thin pool flag for segment %u",
						  lv->name, seg_count);
					inc_error_count;
				}

				if (lv_is_thin_volume(lv)) {
					log_error("LV %s is a thin volume that must not contain thin pool segment %u",
						  lv->name, seg_count);
					inc_error_count;
				}

			}
			if (seg_is_cache_pool(seg) &&
			    !dm_list_empty(&seg->lv->segs_using_this_lv)) {
				switch (seg->cache_mode) {
				case CACHE_MODE_WRITETHROUGH:
				case CACHE_MODE_WRITEBACK:
				case CACHE_MODE_PASSTHROUGH:
					break;
				default:
					log_error("LV %s has invalid cache's feature flag.",
						  lv->name);
					inc_error_count;
				}
				if (!seg->policy_name) {
					log_error("LV %s is missing cache policy name.", lv->name);
					inc_error_count;
				}
			}
			if (seg_is_pool(seg)) {
				if (seg->area_count != 1 ||
				    seg_type(seg, 0) != AREA_LV) {
					log_error("LV %s: %s segment %u is missing a pool data LV",
						  lv->name, seg->segtype->name, seg_count);
					inc_error_count;
				} else if (!(seg2 = first_seg(seg_lv(seg, 0))) || find_pool_seg(seg2) != seg) {
					log_error("LV %s: %s segment %u data LV does not refer back to pool LV",
						  lv->name, seg->segtype->name, seg_count);
					inc_error_count;
				}

				if (!seg->metadata_lv) {
					log_error("LV %s: %s segment %u is missing a pool metadata LV",
						  lv->name, seg->segtype->name, seg_count);
					inc_error_count;
				} else if (!(seg2 = first_seg(seg->metadata_lv)) ||
					   find_pool_seg(seg2) != seg) {
					log_error("LV %s: %s segment %u metadata LV does not refer back to pool LV",
						  lv->name, seg->segtype->name, seg_count);
					inc_error_count;
				}

				if (!validate_pool_chunk_size(lv->vg->cmd, seg->segtype, seg->chunk_size)) {
					log_error("LV %s: %s segment %u has invalid chunk size %u.",
						  lv->name, seg->segtype->name, seg_count, seg->chunk_size);
					inc_error_count;
				}
			} else {
				if (seg->metadata_lv) {
					log_error("LV %s: segment %u must not have pool metadata LV set",
						  lv->name, seg_count);
					inc_error_count;
				}
			}

			if (seg_is_thin_volume(seg)) {
				if (!lv_is_thin_volume(lv)) {
					log_error("LV %s is missing thin volume flag for segment %u",
						  lv->name, seg_count);
					inc_error_count;
				}

				if (lv_is_thin_pool(lv)) {
					log_error("LV %s is a thin pool that must not contain thin volume segment %u",
						  lv->name, seg_count);
					inc_error_count;
				}

				if (!seg->pool_lv) {
					log_error("LV %s: segment %u is missing thin pool LV",
						  lv->name, seg_count);
					inc_error_count;
				} else if (!lv_is_thin_pool(seg->pool_lv)) {
					log_error("LV %s: thin volume segment %u pool LV is not flagged as a pool LV",
						  lv->name, seg_count);
					inc_error_count;
				}

				if (seg->device_id > DM_THIN_MAX_DEVICE_ID) {
					log_error("LV %s: thin volume segment %u has too large device id %u",
						  lv->name, seg_count, seg->device_id);
					inc_error_count;
				}
				if (seg->external_lv && (seg->external_lv->status & LVM_WRITE)) {
					log_error("LV %s: external origin %s is writable.",
						  lv->name, seg->external_lv->name);
					inc_error_count;
				}

				if (seg->merge_lv) {
					if (!lv_is_thin_volume(seg->merge_lv)) {
						log_error("LV %s: thin volume segment %u merging LV %s is not flagged as a thin LV",
							  lv->name, seg_count, seg->merge_lv->name);
						inc_error_count;
					}
					if (!lv_is_merging_origin(seg->merge_lv)) {
						log_error("LV %s: merging LV %s is not flagged as merging.",
							  lv->name, seg->merge_lv->name);
						inc_error_count;
					}
				}
			} else if (seg_is_cache(seg)) {
				if (!lv_is_cache(lv)) {
					log_error("LV %s is missing cache flag for segment %u",
						  lv->name, seg_count);
					inc_error_count;
				}
				if (!seg->pool_lv) {
					log_error("LV %s: segment %u is missing cache_pool LV",
						  lv->name, seg_count);
					inc_error_count;
				}
			} else {
				if (seg->pool_lv) {
					log_error("LV %s: segment %u must not have pool LV set",
						  lv->name, seg_count);
					inc_error_count;
				}
			}
		}

		if (seg_is_snapshot(seg)) {
			if (seg->cow && seg->cow == seg->origin) {
				log_error("LV %s: segment %u has same LV %s for "
					  "both origin and snapshot",
					  lv->name, seg_count, seg->cow->name);
				inc_error_count;
			}
		}

		if (seg_is_replicator(seg) && !check_replicator_segment(seg))
			inc_error_count;

		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) == AREA_UNASSIGNED) {
				log_error("LV %s: segment %u has unassigned "
					  "area %u.",
					  lv->name, seg_count, s);
				inc_error_count;
			} else if (seg_type(seg, s) == AREA_PV) {
				if (!seg_pvseg(seg, s) ||
				    seg_pvseg(seg, s)->lvseg != seg ||
				    seg_pvseg(seg, s)->lv_area != s) {
					log_error("LV %s: segment %u has "
						  "inconsistent PV area %u",
						  lv->name, seg_count, s);
					inc_error_count;
				}
			} else {
				if (!seg_lv(seg, s) ||
				    seg_lv(seg, s)->vg != lv->vg ||
				    seg_lv(seg, s) == lv) {
					log_error("LV %s: segment %u has "
						  "inconsistent LV area %u",
						  lv->name, seg_count, s);
					inc_error_count;
				}

				if (complete_vg && seg_lv(seg, s) &&
				    lv_is_mirror_image(seg_lv(seg, s)) &&
				    (!(seg2 = find_seg_by_le(seg_lv(seg, s),
							    seg_le(seg, s))) ||
				     find_mirror_seg(seg2) != seg)) {
					log_error("LV %s: segment %u mirror "
						  "image %u missing mirror ptr",
						  lv->name, seg_count, s);
					inc_error_count;
				}

/* FIXME I don't think this ever holds?
				if (seg_le(seg, s) != le) {
					log_error("LV %s: segment %u has "
						  "inconsistent LV area %u "
						  "size",
						  lv->name, seg_count, s);
					inc_error_count;
				}
 */
				seg_found = 0;
				dm_list_iterate_items(sl, &seg_lv(seg, s)->segs_using_this_lv)
					if (sl->seg == seg)
						seg_found++;

				if (!seg_found) {
					log_error("LV %s segment %u uses LV %s,"
						  " but missing ptr from %s to %s",
						  lv->name, seg_count,
						  seg_lv(seg, s)->name,
						  seg_lv(seg, s)->name, lv->name);
					inc_error_count;
				} else if (seg_found > 1) {
					log_error("LV %s has duplicated links "
						  "to LV %s segment %u",
						  seg_lv(seg, s)->name,
						  lv->name, seg_count);
					inc_error_count;
				}
			}

			if (complete_vg &&
			    seg_is_mirrored(seg) && !seg_is_raid(seg) &&
			    seg_type(seg, s) == AREA_LV &&
			    seg_lv(seg, s)->le_count != seg->area_len) {
				log_error("LV %s: mirrored LV segment %u has "
					  "wrong size %u (should be %u).",
					  lv->name, s, seg_lv(seg, s)->le_count,
					  seg->area_len);
				inc_error_count;
			}
		}

		le += seg->len;
	}

	dm_list_iterate_items(sl, &lv->segs_using_this_lv) {
		seg = sl->seg;
		seg_found = 0;
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) != AREA_LV)
				continue;
			if (lv == seg_lv(seg, s))
				seg_found++;
			if (seg_is_raid_with_meta(seg) && (lv == seg_metalv(seg, s)))
				seg_found++;
		}
		if (seg_is_replicator_dev(seg)) {
			dm_list_iterate_items(rsite, &seg->replicator->rsites) {
				dm_list_iterate_items(rdev, &rsite->rdevices) {
					if (lv == rdev->lv || lv == rdev->slog)
						seg_found++;
				}
			}
			if (lv == seg->replicator)
				seg_found++;
		}
		if (seg_is_replicator(seg) && lv == seg->rlog_lv)
			seg_found++;
		if (seg->log_lv == lv)
			seg_found++;
		if (seg->metadata_lv == lv || seg->pool_lv == lv)
			seg_found++;
		if (seg_is_thin_volume(seg) && (seg->origin == lv || seg->external_lv == lv))
			seg_found++;

		if (!seg_found) {
			log_error("LV %s is used by LV %s:%" PRIu32 "-%" PRIu32
				  ", but missing ptr from %s to %s",
				  lv->name, seg->lv->name, seg->le,
				  seg->le + seg->len - 1,
				  seg->lv->name, lv->name);
			inc_error_count;
		} else if (seg_found != sl->count) {
			log_error("Reference count mismatch: LV %s has %u "
				  "links to LV %s:%" PRIu32 "-%" PRIu32
				  ", which has %u links",
				  lv->name, sl->count, seg->lv->name, seg->le,
				  seg->le + seg->len - 1, seg_found);
			inc_error_count;
		}

		seg_found = 0;
		dm_list_iterate_items(seg2, &seg->lv->segments)
			if (seg == seg2) {
				seg_found++;
				break;
			}

		if (!seg_found) {
			log_error("LV segment %s:%" PRIu32 "-%" PRIu32
				  " is incorrectly listed as being used by LV %s",
				  seg->lv->name, seg->le, seg->le + seg->len - 1,
				  lv->name);
			inc_error_count;
		}
	}

	dm_list_iterate_items(glvl, &lv->indirect_glvs) {
		if (glvl->glv->is_historical) {
			if (glvl->glv->historical->indirect_origin != lv->this_glv) {
				log_error("LV %s is indirectly used by historical LV %s"
					  "but that historical LV does not point back to LV %s",
					   lv->name, glvl->glv->historical->name, lv->name);
				inc_error_count;
			}
		} else {
			if (!(seg = first_seg(glvl->glv->live)) ||
			    seg->indirect_origin != lv->this_glv) {
				log_error("LV %s is indirectly used by LV %s"
					  "but that LV does not point back to LV %s",
					  lv->name, glvl->glv->live->name, lv->name);
				inc_error_count;
			}
		}
	}

	if (le != lv->le_count) {
		log_error("LV %s: inconsistent LE count %u != %u",
			  lv->name, le, lv->le_count);
		inc_error_count;
	}

out:
	return !error_count;
}

/*
 * Split the supplied segment at the supplied logical extent
 * NB Use LE numbering that works across stripes PV1: 0,2,4 PV2: 1,3,5 etc.
 */
static int _lv_split_segment(struct logical_volume *lv, struct lv_segment *seg,
			     uint32_t le)
{
	struct lv_segment *split_seg;
	uint32_t s;
	uint32_t offset = le - seg->le;
	uint32_t area_offset;

	if (!seg_can_split(seg)) {
		log_error("Unable to split the %s segment at LE %" PRIu32
			  " in LV %s", lvseg_name(seg), le, lv->name);
		return 0;
	}

	/* Clone the existing segment */
	if (!(split_seg = alloc_lv_segment(seg->segtype,
					   seg->lv, seg->le, seg->len,
					   seg->status, seg->stripe_size,
					   seg->log_lv,
					   seg->area_count, seg->area_len,
					   seg->chunk_size, seg->region_size,
					   seg->extents_copied, seg->pvmove_source_seg))) {
		log_error("Couldn't allocate cloned LV segment.");
		return 0;
	}

	if (!str_list_dup(lv->vg->vgmem, &split_seg->tags, &seg->tags)) {
		log_error("LV segment tags duplication failed");
		return 0;
	}

	/* In case of a striped segment, the offset has to be / stripes */
	area_offset = offset;
	if (seg_is_striped(seg))
		area_offset /= seg->area_count;

	split_seg->area_len -= area_offset;
	seg->area_len = area_offset;

	split_seg->len -= offset;
	seg->len = offset;

	split_seg->le = seg->le + seg->len;

	/* Adjust the PV mapping */
	for (s = 0; s < seg->area_count; s++) {
		seg_type(split_seg, s) = seg_type(seg, s);

		/* Split area at the offset */
		switch (seg_type(seg, s)) {
		case AREA_LV:
			if (!set_lv_segment_area_lv(split_seg, s, seg_lv(seg, s),
						    seg_le(seg, s) + seg->area_len, 0))
				return_0;
			log_debug_alloc("Split %s:%u[%u] at %u: %s LE %u", lv->name,
					seg->le, s, le, seg_lv(seg, s)->name,
					seg_le(split_seg, s));
			break;

		case AREA_PV:
			if (!(seg_pvseg(split_seg, s) =
			     assign_peg_to_lvseg(seg_pv(seg, s),
						 seg_pe(seg, s) +
						     seg->area_len,
						 seg_pvseg(seg, s)->len -
						     seg->area_len,
						 split_seg, s)))
				return_0;
			log_debug_alloc("Split %s:%u[%u] at %u: %s PE %u", lv->name,
					seg->le, s, le,
					dev_name(seg_dev(seg, s)),
					seg_pe(split_seg, s));
			break;

		case AREA_UNASSIGNED:
			log_error("Unassigned area %u found in segment", s);
			return 0;
		}
	}

	/* Add split off segment to the list _after_ the original one */
	dm_list_add_h(&seg->list, &split_seg->list);

	return 1;
}

/*
 * Ensure there's a segment boundary at the given logical extent
 */
int lv_split_segment(struct logical_volume *lv, uint32_t le)
{
	struct lv_segment *seg;

	if (!(seg = find_seg_by_le(lv, le))) {
		log_error("Segment with extent %" PRIu32 " in LV %s not found",
			  le, lv->name);
		return 0;
	}

	/* This is a segment start already */
	if (le == seg->le)
		return 1;

	if (!_lv_split_segment(lv, seg, le))
		return_0;

	if (!vg_validate(lv->vg))
		return_0;

	return 1;
}
