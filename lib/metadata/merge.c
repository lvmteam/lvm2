/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "metadata.h"

/*
 * Returns success if the segments were
 * successfully merged.  If the do merge, 'first'
 * will be adjusted to contain both areas.
 */
static int _merge(struct lv_segment *first, struct lv_segment *second)
{
	unsigned int s;
	uint32_t width;

	if (!first ||
	    (first->type != SEG_STRIPED) ||
	    (first->type != second->type) ||
	    (first->area_count != second->area_count) ||
	    (first->stripe_size != second->stripe_size))
		return 0;

	for (s = 0; s < first->area_count; s++) {
		width = first->area_len;

		/* FIXME Relax this to first type != second type ? */
		if (first->area[s].type != AREA_PV ||
		    second->area[s].type != AREA_PV)
			return 0;

		if ((first->area[s].u.pv.pv != second->area[s].u.pv.pv) ||
		    (first->area[s].u.pv.pe + width != second->area[s].u.pv.pe))
			return 0;
	}

	/* we should merge */
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

int lv_check_segments(struct logical_volume *lv)
{
	return 1;
}
