/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "ll-activate.h"
#include "lvm-string.h"
#include "log.h"

#include <libdevmapper.h>
#include <linux/kdev_t.h>


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

int _load(const char *name, struct logical_volume *lv, int task)
{
	int r = 0;
	struct dm_task *dmt;
	struct list *segh;
	struct stripe_segment *seg;

	log_very_verbose("Generating devmapper parameters for %s", lv->name);
	if (!(dmt = setup_dm_task(name, task))) {
		stack;
		return 0;
	}

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct stripe_segment);
		if (!_emit_target(dmt, seg)) {
			log_error("Unable to activate logical volume '%s'",
				lv->name);
			goto out;
		}
	}

	if (!((lv->status & LVM_WRITE) && (lv->vg->status & LVM_WRITE))) {
	    	if (!dm_task_set_ro(dmt)) {
			log_error("Failed to set %s read-only during "
				  "activation.", lv->name);
			goto out;
		} else
			log_very_verbose("Activating %s read-only", lv->name);
	}

	if (lv->minor >= 0) {
		if (!dm_task_set_minor(dmt, MINOR(lv->minor))) {
			log_error("Failed to set minor number for %s to %d "
				  "during activation.", lv->name, lv->minor);
			goto out;
		} else
			log_very_verbose("Set minor number for %s to %d.",
					 lv->name, lv->minor);
	}

	if (!(r = dm_task_run(dmt)))
		stack;

	log_verbose("Logical volume %s%s activated", lv->name,
		    r == 1 ? "" : " not");

 out:
	dm_task_destroy(dmt);
	return r;
}

int device_create_lv(const char *name, struct logical_volume *lv, int minor)
{
	log_very_verbose("Activating %s", name);
	return _load(name, lv, DM_DEVICE_CREATE);
}

int device_reload_lv(const char *name, struct logical_volume *lv)
{
	log_very_verbose("Reactivating %s", name);
	return _load(name, lv, DM_DEVICE_RELOAD);
}
