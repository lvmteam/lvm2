/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "metadata.h"

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

