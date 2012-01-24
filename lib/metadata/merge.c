/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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
	struct lv_segment *current, *prev = NULL;

	if (lv->status & LOCKED || lv->status & PVMOVE)
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
 * Verify that an LV's segments are consecutive, complete and don't overlap.
 */
int check_lv_segments(struct logical_volume *lv, int complete_vg)
{
	struct lv_segment *seg, *seg2;
	uint32_t le = 0;
	unsigned seg_count = 0, seg_found;
	uint32_t area_multiplier, s;
	struct seg_list *sl;
	int error_count = 0;
	struct replicator_site *rsite;
	struct replicator_device *rdev;

	/* Check LV flags match first segment type */
	if (complete_vg) {
		if (lv_is_thin_volume(lv) &&
		    (!(seg2 = first_seg(lv)) || !seg_is_thin_volume(seg2))) {
			log_error("LV %s is thin volume without first thin volume segment",
				  lv->name);
			inc_error_count;
		}

		if (lv_is_thin_pool(lv) &&
		    (!(seg2 = first_seg(lv)) || !seg_is_thin_pool(seg2))) {
			log_error("LV %s is thin pool without first thin pool segment",
				  lv->name);
			inc_error_count;
		}

		if (lv_is_thin_pool_data(lv) &&
		    (!(seg2 = first_seg(lv)) || !(seg2 = find_pool_seg(seg2)) ||
		     seg2->area_count != 1 || seg_type(seg2, 0) != AREA_LV ||
		     seg_lv(seg2, 0) != lv)) {
			log_error("LV %s: segment 1 pool data LV does not point back to same LV",
				  lv->name);
			inc_error_count;
		}

		if (lv_is_thin_pool_metadata(lv) &&
		    (!(seg2 = first_seg(lv)) || !(seg2 = find_pool_seg(seg2)) ||
		     seg2->metadata_lv != lv)) {
			log_error("LV %s: segment 1 pool metadata LV does not point back to same LV",
				  lv->name);
			inc_error_count;
		}
	}

	dm_list_iterate_items(seg, &lv->segments) {
		seg_count++;
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

		if (complete_vg && seg->log_lv &&
		    !seg_is_mirrored(seg) && !(seg->status & RAID_IMAGE)) {
			log_error("LV %s: segment %u log LV %s is not a "
				  "mirror log or a RAID image",
				  lv->name, seg_count, seg->log_lv->name);
			inc_error_count;
		}

		/*
		 * Check mirror log - which is attached to the mirrored seg
		 */
		if (complete_vg && seg->log_lv && seg_is_mirrored(seg)) {
			if (!(seg->log_lv->status & MIRROR_LOG)) {
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

		if (complete_vg && seg->status & MIRROR_IMAGE) {
			if (!find_mirror_seg(seg) ||
			    !seg_is_mirrored(find_mirror_seg(seg))) {
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

				if (seg->area_count != 1 || seg_type(seg, 0) != AREA_LV) {
					log_error("LV %s: thin pool segment %u is missing a pool data LV",
						  lv->name, seg_count);
					inc_error_count;
				} else if (!(seg2 = first_seg(seg_lv(seg, 0))) || find_pool_seg(seg2) != seg) {
					log_error("LV %s: thin pool segment %u data LV does not refer back to pool LV",
						  lv->name, seg_count);
					inc_error_count;
				}

				if (!seg->metadata_lv) {
					log_error("LV %s: thin pool segment %u is missing a pool metadata LV",
						  lv->name, seg_count);
					inc_error_count;
				} else if (!(seg2 = first_seg(seg->metadata_lv)) ||
					   find_pool_seg(seg2) != seg) {
					log_error("LV %s: thin pool segment %u metadata LV does not refer back to pool LV",
						  lv->name, seg_count);
					inc_error_count;
				}

				if (seg->chunk_size < DM_THIN_MIN_DATA_BLOCK_SIZE ||
				    seg->chunk_size > DM_THIN_MAX_DATA_BLOCK_SIZE) {
					log_error("LV %s: thin pool segment %u  chunk size %d is out of range",
						  lv->name, seg_count, seg->chunk_size);
					inc_error_count;
				}
			} else {
				if (seg->metadata_lv) {
					log_error("LV %s: segment %u must not have thin pool metadata LV set",
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
					log_error("LV %s: thin volume segment %u has too large device id %d",
						  lv->name, seg_count, seg->device_id);
					inc_error_count;
				}
			} else {
				if (seg->pool_lv) {
					log_error("LV %s: segment %u must not have thin pool LV set",
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
				    (seg_lv(seg, s)->status & MIRROR_IMAGE) &&
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
					log_error("LV %s segment %d uses LV %s,"
						  " but missing ptr from %s to %s",
						  lv->name, seg_count,
						  seg_lv(seg, s)->name,
						  seg_lv(seg, s)->name, lv->name);
					inc_error_count;
				} else if (seg_found > 1) {
					log_error("LV %s has duplicated links "
						  "to LV %s segment %d",
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
			if (seg_is_raid(seg) && (lv == seg_metalv(seg, s)))
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
		if (seg_is_thin_volume(seg) && seg->origin == lv)
			seg_found++;
		if (!seg_found) {
			log_error("LV %s is used by LV %s:%" PRIu32 "-%" PRIu32
				  ", but missing ptr from %s to %s",
				  lv->name, seg->lv->name, seg->le,
				  seg->le + seg->len - 1,
				  seg->lv->name, lv->name);
			inc_error_count;
		} else if (seg_found != sl->count) {
			log_error("Reference count mismatch: LV %s has %d "
				  "links to LV %s:%" PRIu32 "-%" PRIu32
				  ", which has %d links",
				  lv->name, sl->count, seg->lv->name, seg->le,
				  seg->le + seg->len - 1, seg_found);
			inc_error_count;
		}

		seg_found = 0;
		dm_list_iterate_items(seg2, &seg->lv->segments)
			if (sl->seg == seg2) {
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
			  " in LV %s", seg->segtype->name, le, lv->name);
		return 0;
	}

	/* Clone the existing segment */
	if (!(split_seg = alloc_lv_segment(seg->segtype,
					   seg->lv, seg->le, seg->len,
					   seg->status, seg->stripe_size,
					   seg->log_lv, seg->pool_lv,
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
			log_debug("Split %s:%u[%u] at %u: %s LE %u", lv->name,
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
			log_debug("Split %s:%u[%u] at %u: %s PE %u", lv->name,
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
