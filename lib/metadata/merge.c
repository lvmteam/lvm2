/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "metadata.h"
#include "toolcontext.h"
#include "lv_alloc.h"
#include "str_list.h"
#include "segtypes.h"

/*
 * Attempt to merge two adjacent segments.
 * Currently only supports striped segments on AREA_PV.
 * Returns success if successful, in which case 'first' 
 * gets adjusted to contain both areas.
 */
static int _merge(struct lv_segment *first, struct lv_segment *second)
{
	if (!first || !second || first->segtype != second->segtype ||
	    !first->segtype->ops->merge_segments) return 0;

	return first->segtype->ops->merge_segments(first, second);
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

	if (!(seg->segtype->flags & SEG_CAN_SPLIT)) {
		log_error("Unable to split the %s segment at LE %" PRIu32
			  " in LV %s", seg->segtype->name, le, lv->name);
		return 0;
	}

	/* Clone the existing segment */
	if (!(split_seg = alloc_lv_segment(lv->vg->cmd->mem, seg->area_count))) {
		log_error("Couldn't allocate new LV segment.");
		return 0;
	}

	len = sizeof(*seg) + (seg->area_count * sizeof(seg->area[0]));
	memcpy(split_seg, seg, len);

	if (!str_list_dup(lv->vg->cmd->mem, &split_seg->tags, &seg->tags)) {
		log_error("LV segment tags duplication failed");
		return 0;
	}

	/* In case of a striped segment, the offset has to be / stripes */
	if (seg->segtype->flags & SEG_AREAS_STRIPED)
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
