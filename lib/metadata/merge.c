/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "log.h"
#include "merge.h"

/*
 * Returns success if the segments were
 * successfully merged.  If the do merge, 'first'
 * will be adjusted to contain both areas.
 */
static int _merge(struct stripe_segment *first, struct stripe_segment *second)
{
	int s;
	uint32_t width;

	if (!first ||
	    (first->stripes != second->stripes) ||
	    (first->stripe_size != second->stripe_size))
		return 0;

	for (s = 0; s < first->stripes; s++) {
		width = first->len / first->stripes;

		if ((first->area[s].pv != second->area[s].pv) ||
		    (first->area[s].pe + width != second->area[s].pe))
			return 0;
	}

	/* we should merge */
	first->len += second->len;

	return 1;
}

int merge_segments(struct logical_volume *lv)
{
	struct list *segh;
	struct stripe_segment *current, *prev = NULL;

	list_iterate (segh, &lv->segments) {
		current = list_item(segh, struct stripe_segment);

		if (_merge(prev, current))
			list_del(&current->list);
		else
			prev = current;
	}

	return 1;
}

