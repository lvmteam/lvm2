/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "metadata.h"
#include "toolcontext.h"
#include "lv_alloc.h"

/*
 * Test whether two segments could be merged by the current merging code
 */
static int _segments_compatible(struct lv_segment *first,
				struct lv_segment *second)
{
	uint32_t width;
	unsigned s;

	/* FIXME Relax the seg type restriction */
	if (!first || !second ||
	    (first->type != SEG_STRIPED) || (second->type != first->type) ||
	    (first->area_count != second->area_count) ||
	    (first->stripe_size != second->stripe_size))
		return 0;

	for (s = 0; s < first->area_count; s++) {

		/* FIXME Relax this to first area type != second area type */
		/*       plus the additional AREA_LV checks needed */
		if ((first->area[s].type != AREA_PV) ||
		    (second->area[s].type != AREA_PV))
			return 0;

		width = first->area_len;

		if ((first->area[s].u.pv.pv != second->area[s].u.pv.pv) ||
		    (first->area[s].u.pv.pe + width != second->area[s].u.pv.pe))
			return 0;
	}

	return 1;
}

/*
 * Attempt to merge two adjacent segments.
 * Currently only supports SEG_STRIPED on AREA_PV.
 * Returns success if successful, in which case 'first' 
 * gets adjusted to contain both areas.
 */
static int _merge(struct lv_segment *first, struct lv_segment *second)
{

	if (!_segments_compatible(first, second))
		return 0;

	first->len += second->len;
	first->area_len += second->area_len;

	return 1;
}

int lv_merge_segments(struct logical_volume *lv)
{
	struct list *segh, *t;
	struct lv_segment *current, *prev = NULL;

	list_iterate_safe(segh, t, &lv->segments) {
		current = list_item(segh, struct lv_segment);

		if (_merge(prev, current))
			list_del(&current->list);
		else
			prev = current;
	}

	return 1;
}

/*
 * Verify that an LV's segments are consecutive, complete and don't overlap.
 */
int lv_check_segments(struct logical_volume *lv)
{
	struct lv_segment *seg;
	uint32_t le = 0;
	unsigned seg_count = 0;

	list_iterate_items(seg, &lv->segments) {
		seg_count++;
		if (seg->le != le) {
			log_error("LV %s invalid: segment %u should begin at "
				  "LE %" PRIu32 " (found %" PRIu32 ").",
				  lv->name, seg_count, le, seg->le);
			return 0;
		}

		le += seg->len;
	}

	return 1;
}

/*
 * Split the supplied segment at the supplied logical extent
 */
static int _lv_split_segment(struct logical_volume *lv, struct lv_segment *seg,
			     uint32_t le)
{
	size_t len;
	struct lv_segment *split_seg;
	uint32_t s;
	uint32_t offset = le - seg->le;

	if (seg->type == SEG_SNAPSHOT) {
		log_error("Unable to split the snapshot segment at LE %" PRIu32
			  " in LV %s", le, lv->name);
		return 0;
	}

	/* Clone the existing segment */
	if (!(split_seg = alloc_lv_segment(lv->vg->cmd->mem,
					   seg->area_count))) {
		log_error("Couldn't allocate new LV segment.");
		return 0;
	}

	len = sizeof(*seg) + (seg->area_count * sizeof(seg->area[0]));
	memcpy(split_seg, seg, len);

	/* In case of a striped segment, the offset has to be / stripes */
	if (seg->type == SEG_STRIPED)
		offset /= seg->area_count;

	/* Adjust the PV mapping */
	for (s = 0; s < seg->area_count; s++) {
		/* Split area at the offset */
		switch (seg->area[s].type) {
		case AREA_LV:
			split_seg->area[s].u.lv.le =
			    seg->area[s].u.lv.le + offset;
			break;

		case AREA_PV:
			split_seg->area[s].u.pv.pe =
			    seg->area[s].u.pv.pe + offset;
			break;

		default:
			log_error("Unrecognised segment type %u",
				  seg->area[s].type);
			return 0;
		}
	}

	split_seg->area_len = seg->area_len - offset;
	seg->area_len = offset;

	/* Add split off segment to the list _after_ the original one */
	list_add_h(&seg->list, &split_seg->list);

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

	if (!_lv_split_segment(lv, seg, le)) {
		stack;
		return 0;
	}

	return 1;
}

