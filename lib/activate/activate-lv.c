/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "ll-activate.h"
#include "lvm-string.h"
#include "log.h"

#include <libdevmapper.h>

/*
 * Emit a target for a given segment.
 * FIXME: tidy this function.
 */
static int _emit_target(struct dm_task *dmt, struct stripe_segment *seg)
{
	char params[1024];
	uint64_t esize = seg->lv->vg->extent_size;
	uint32_t s, stripes = seg->stripes;
	int w = 0, tw = 0, error = 0;
	const char *no_space =
		"Insufficient space to write target parameters.";
	char *filler = "/dev/ioerror";
	char *target;

	if (stripes == 1) {
		if (!seg->area[0].pv) {
			target = "error";
			error = 1;
		}
		else
			target = "linear";
	}

	if (stripes > 1) {
		target = "striped";
		tw = lvm_snprintf(params, sizeof(params), "%u %u ",
			      stripes, seg->stripe_size);

		if (tw < 0) {
			log_err(no_space);
			return 0;
		}

		w = tw;
	}

	if (!error) {
		for (s = 0; s < stripes; s++, w += tw) {
			if (!seg->area[s].pv)
				tw = lvm_snprintf(
					params + w, sizeof(params) - w,
			      		"%s 0%s", filler,
			      		s == (stripes - 1) ? "" : " ");
			else
				tw = lvm_snprintf(
					params + w, sizeof(params) - w,
			      		"%s %" PRIu64 "%s",
					dev_name(seg->area[s].pv->dev),
			      		(seg->area[s].pv->pe_start +
			         	 (esize * seg->area[s].pe)),
			      		s == (stripes - 1) ? "" : " ");

			if (tw < 0) {
				log_err(no_space);
				return 0;
			}
		}
	}

	log_very_verbose("Adding target: %" PRIu64 " %" PRIu64 " %s %s",
		   esize * seg->le, esize * seg->len,
		   target, params);

	if (!dm_task_add_target(dmt, esize * seg->le, esize * seg->len,
				target, params)) {
		stack;
		return 0;
	}

	return 1;
}

int device_populate_lv(struct dm_task *dmt, struct logical_volume *lv)
{
	struct list *segh;
	struct stripe_segment *seg;

	log_very_verbose("Generating devmapper table for %s", lv->name);
	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct stripe_segment);
		if (!_emit_target(dmt, seg)) {
			log_error("Unable to build table for '%s'", lv->name);
			return 0;
		}
	}

	return 1;
}
