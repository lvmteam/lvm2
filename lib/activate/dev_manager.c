/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.  
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
#include "str_list.h"
#include "dev_manager.h"
#include "pool.h"
#include "hash.h"
#include "lvm-string.h"
#include "fs.h"
#include "defaults.h"
#include "segtype.h"
#include "display.h"
#include "toolcontext.h"
#include "targets.h"
#include "config.h"

#include <libdevmapper.h>
#include <limits.h>
#include <dirent.h>

/*
 * Algorithm
 * ---------
 *
 * 1) Examine dm directory, and store details of active mapped devices
 *    in the VG.  Indexed by lvid-layer. (_scan_existing_devices)
 *
 * 2) Build lists of visible devices that need to be left in each state:
 *    active, reloaded, suspended.
 *
 * 3) Run through these lists and set the appropriate marks on each device
 *    and its dependencies.
 *
 * 4) Add layers not marked active to remove_list for removal at the end.
 *
 * 5) Remove unmarked layers from core.
 *
 * 6) Activate remaining layers, recursing to handle dependedncies and
 *    skipping any that already exist unless they are marked as needing
 *    reloading.
 *
 * 7) Remove layers in the remove_list.  (_remove_old_layers)
 *
 */

#define MAX_TARGET_PARAMSIZE 50000

enum {
	ACTIVE = 0,
	RELOAD = 1,
	VISIBLE = 2,
	READWRITE = 3,
	SUSPENDED = 4,
	NOPROPAGATE = 5,
	TOPLEVEL = 6,
	REMOVE = 7,
	RESUME_IMMEDIATE = 8
};

typedef enum {
	ACTIVATE,
	DEACTIVATE,
	SUSPEND,
	RESUME
} action_t;

struct dev_layer {
	char *name;

	int flags;

	/*
	 * Setup the dm_task.
	 */
	int (*populate) (struct dev_manager * dm,
			 struct dm_task * dmt, struct dev_layer * dl);
	struct dm_info info;

	/* lvid plus layer */
	const char *dlid;

	struct logical_volume *lv;

	/*
	 * Devices that must be created before this one can be created.
	 * Reloads get propagated to this list.  Holds str_lists.
	 */
	struct list pre_create;

	/* Inverse of pre_create */
	struct list pre_suspend;

};

struct dl_list {
	struct list list;
	struct dev_layer *dl;
};

static const char *stripe_filler = NULL;

struct dev_manager {
	struct pool *mem;

	struct cmd_context *cmd;

	const char *stripe_filler;
	void *target_state;
	uint32_t pvmove_mirror_count;

	char *vg_name;

	/*
	 * list of struct lv_list, contains lvs that we wish to
	 * be active after execution.
	 */
	struct list active_list;

	/*
	 * Layers that need reloading.
	 */
	struct list reload_list;

	/*
	 * Layers that need suspending.
	 */
	struct list suspend_list;

	/*
	 * Layers that will need removing after activation.
	 */
	struct list remove_list;

	struct hash_table *layers;
};

/*
 * Functions to manage the flags.
 */
static inline int _get_flag(struct dev_layer *dl, int bit)
{
	return (dl->flags & (1 << bit)) ? 1 : 0;
}

static inline void _set_flag(struct dev_layer *dl, int bit)
{
	dl->flags |= (1 << bit);
}

static inline void _clear_flag(struct dev_layer *dl, int bit)
{
	dl->flags &= ~(1 << bit);
}

static char *_build_dlid(struct pool *mem, const char *lvid, const char *layer)
{
	char *dlid;
	size_t len;

	if (!layer)
		layer = "";

	len = strlen(lvid) + strlen(layer) + 2;

	if (!(dlid = pool_alloc(mem, len))) {
		stack;
		return NULL;
	}

	sprintf(dlid, "%s%s%s", lvid, (*layer) ? "-" : "", layer);

	return dlid;
}

/*
 * Low level device-layer operations.
 */
static struct dm_task *_setup_task(const char *name, const char *uuid,
				   uint32_t *event_nr, int task)
{
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task))) {
		stack;
		return NULL;
	}

	if (name)
		dm_task_set_name(dmt, name);

	if (uuid && *uuid)
		dm_task_set_uuid(dmt, uuid);

	if (event_nr)
		dm_task_set_event_nr(dmt, *event_nr);

	return dmt;
}

static int _info_run(const char *name, const char *uuid, struct dm_info *info,
		     int mknodes, int with_open_count, struct pool *mem,
		     char **uuid_out)
{
	int r = 0;
	struct dm_task *dmt;
	const char *u;
	int dmtask;

	dmtask = mknodes ? DM_DEVICE_MKNODES : DM_DEVICE_INFO;

	if (!(dmt = _setup_task(name, uuid, 0, dmtask))) {
		stack;
		return 0;
	}

	if (!with_open_count)
		if (!dm_task_no_open_count(dmt))
			log_error("Failed to disable open_count");

	if (!dm_task_run(dmt)) {
		stack;
		goto out;
	}

	if (!dm_task_get_info(dmt, info)) {
		stack;
		goto out;
	}

	if (info->exists && uuid_out) {
		if (!(u = dm_task_get_uuid(dmt))) {
			stack;
			goto out;
		}
		*uuid_out = pool_strdup(mem, u);
	}
	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _info(const char *name, const char *uuid, int mknodes,
		 int with_open_count, struct dm_info *info,
		 struct pool *mem, char **uuid_out)
{
	if (!mknodes && uuid && *uuid &&
	    _info_run(NULL, uuid, info, 0, with_open_count, mem, uuid_out) &&
	    	      info->exists)
		return 1;

	if (name)
		return _info_run(name, NULL, info, mknodes, with_open_count,
				 mem, uuid_out);

	return 0;
}

/* FIXME Interface must cope with multiple targets */
static int _status_run(const char *name, const char *uuid,
		       unsigned long long *s, unsigned long long *l,
		       char **t, uint32_t t_size, char **p, uint32_t p_size)
{
	int r = 0;
	struct dm_task *dmt;
	void *next = NULL;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;

	if (!(dmt = _setup_task(name, uuid, 0, DM_DEVICE_STATUS))) {
		stack;
		return 0;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!dm_task_run(dmt)) {
		stack;
		goto out;
	}

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &type, &params);
		if (type) {
			*s = start;
			*l = length;
			/* Make sure things are null terminated */
			strncpy(*t, type, t_size);
			(*t)[t_size - 1] = '\0';
			strncpy(*p, params, p_size);
			(*p)[p_size - 1] = '\0';

			r = 1;
			/* FIXME Cope with multiple targets! */
			break;
		}

	} while (next);

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _status(const char *name, const char *uuid,
		   unsigned long long *start, unsigned long long *length,
		   char **type, uint32_t type_size, char **params,
		   uint32_t param_size) __attribute__ ((unused));

static int _status(const char *name, const char *uuid,
		   unsigned long long *start, unsigned long long *length,
		   char **type, uint32_t type_size, char **params,
		   uint32_t param_size)
{
	if (uuid && *uuid && _status_run(NULL, uuid, start, length, type,
					 type_size, params, param_size)
	    && *params)
		return 1;

	if (name && _status_run(name, NULL, start, length, type, type_size,
				params, param_size))
		return 1;

	return 0;
}

static int _percent_run(struct dev_manager *dm, const char *name,
			const char *uuid,
			const char *target_type, int wait,
			struct logical_volume *lv, float *percent,
			uint32_t *event_nr)
{
	int r = 0;
	struct dm_task *dmt;
	struct dm_info info;
	void *next = NULL;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;
	struct list *segh = &lv->segments;
	struct lv_segment *seg = NULL;
	struct segment_type *segtype;

	uint64_t total_numerator = 0, total_denominator = 0;

	*percent = -1;

	if (!(dmt = _setup_task(name, uuid, event_nr,
				wait ? DM_DEVICE_WAITEVENT : DM_DEVICE_STATUS))) {
		stack;
		return 0;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!dm_task_run(dmt)) {
		stack;
		goto out;
	}

	if (!dm_task_get_info(dmt, &info) || !info.exists) {
		stack;
		goto out;
	}

	if (event_nr)
		*event_nr = info.event_nr;

	do {
		next = dm_get_next_target(dmt, next, &start, &length, &type,
					  &params);
		if (lv) {
			if (!(segh = list_next(&lv->segments, segh))) {
				log_error("Number of segments in active LV %s "
					  "does not match metadata", lv->name);
				goto out;
			}
			seg = list_item(segh, struct lv_segment);
		}

		if (!type || !params || strcmp(type, target_type))
			continue;

		if (!(segtype = get_segtype_from_string(dm->cmd, type)))
			continue;

		if (segtype->ops->target_percent &&
		    !segtype->ops->target_percent(&dm->target_state, dm->mem,
						  dm->cmd->cft, seg, params,
						  &total_numerator,
						  &total_denominator,
						  percent)) {
			stack;
			goto out;
		}

	} while (next);

	if (lv && (segh = list_next(&lv->segments, segh))) {
		log_error("Number of segments in active LV %s does not "
			  "match metadata", lv->name);
		goto out;
	}

	if (total_denominator)
		*percent = (float) total_numerator *100 / total_denominator;
	else if (*percent < 0)
		*percent = 100;

	log_debug("LV percent: %f", *percent);
	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _percent(struct dev_manager *dm, const char *name, const char *uuid,
		    const char *target_type, int wait,
		    struct logical_volume *lv, float *percent,
		    uint32_t *event_nr)
{
	if (uuid && *uuid
	    && _percent_run(dm, NULL, uuid, target_type, wait, lv, percent,
			    event_nr))
		return 1;

	if (name && _percent_run(dm, name, NULL, target_type, wait, lv, percent,
				 event_nr))
		return 1;

	return 0;
}

static int _rename(struct dev_manager *dm, struct dev_layer *dl, char *newname)
{
	int r = 1;
	struct dm_task *dmt;
	char *vgname, *lvname, *layer;

	if (!split_dm_name(dm->mem, dl->name, &vgname, &lvname, &layer)) {
		log_error("Couldn't split up dm layer name %s", dl->name);
		return 0;
	}

	log_verbose("Renaming %s to %s", dl->name, newname);

	if (!(dmt = _setup_task(dl->name, NULL, 0, DM_DEVICE_RENAME))) {
		stack;
		return 0;
	}

	if (!dm_task_set_newname(dmt, newname)) {
		stack;
		r = 0;
		goto out;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!(r = dm_task_run(dmt))) {
		log_error("Couldn't rename device '%s'.", dl->name);
		goto out;
	}

	if (r && _get_flag(dl, VISIBLE))
		fs_rename_lv(dl->lv, newname, lvname);

	dl->name = newname;

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _suspend_or_resume(const char *name, action_t suspend)
{
	int r;
	struct dm_task *dmt;
	int sus = (suspend == SUSPEND) ? 1 : 0;
	int task = sus ? DM_DEVICE_SUSPEND : DM_DEVICE_RESUME;

	log_very_verbose("%s %s", sus ? "Suspending" : "Resuming", name);
	if (!(dmt = _setup_task(name, NULL, 0, task))) {
		stack;
		return 0;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!(r = dm_task_run(dmt)))
		log_error("Couldn't %s device '%s'", sus ? "suspend" : "resume",
			  name);

	dm_task_destroy(dmt);
	return r;
}

static int _suspend(struct dev_layer *dl)
{
	if (!dl->info.exists || dl->info.suspended)
		return 1;

	if (!_suspend_or_resume(dl->name, SUSPEND)) {
		stack;
		return 0;
	}

	dl->info.suspended = 1;
	return 1;
}

static int _resume(struct dev_layer *dl)
{
	if (!dl->info.exists || !dl->info.suspended)
		return 1;

	if (!_suspend_or_resume(dl->name, RESUME)) {
		stack;
		return 0;
	}

	dl->info.suspended = 0;
	return 1;
}

static int _load(struct dev_manager *dm, struct dev_layer *dl, int task)
{
	int r = 1;
	struct dm_task *dmt;

	log_verbose("Loading %s", dl->name);
	if (!(dmt = _setup_task(task == DM_DEVICE_CREATE ? dl->name : NULL,
				dl->dlid, 0, task))) {
		stack;
		return 0;
	}

	/*
	 * Populate the table.
	 */
	if (!dl->populate(dm, dmt, dl)) {
		log_error("Couldn't populate device '%s'.", dl->name);
		r = 0;
		goto out;
	}

	/*
	 * Do we want a specific device number ?
	 */
	if (dl->lv->major >= 0 && _get_flag(dl, VISIBLE)) {
		if (!dm_task_set_major(dmt, dl->lv->major)) {
			log_error("Failed to set major number for %s to %d "
				  "during activation.", dl->name,
				  dl->lv->major);
			goto out;
		} else
			log_very_verbose("Set major number for %s to %d.",
					 dl->name, dl->lv->major);
	}

	if (dl->lv->minor >= 0 && _get_flag(dl, VISIBLE)) {
		if (!dm_task_set_minor(dmt, dl->lv->minor)) {
			log_error("Failed to set minor number for %s to %d "
				  "during activation.", dl->name,
				  dl->lv->minor);
			goto out;
		} else
			log_very_verbose("Set minor number for %s to %d.",
					 dl->name, dl->lv->minor);
	}

	if (!_get_flag(dl, READWRITE)) {
		if (!dm_task_set_ro(dmt)) {
			log_error("Failed to set %s read-only during "
				  "activation.", dl->name);
			goto out;
		} else
			log_very_verbose("Activating %s read-only", dl->name);
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!(r = dm_task_run(dmt))) {
		log_error("Couldn't load device '%s'.", dl->name);
		if ((dl->lv->minor >= 0 || dl->lv->major >= 0) &&
		    _get_flag(dl, VISIBLE))
			    log_error("Perhaps the persistent device number "
				      "%d:%d is already in use?",
				      dl->lv->major, dl->lv->minor);
	}

	if (!dm_task_get_info(dmt, &dl->info)) {
		stack;
		r = 0;
		goto out;
	}

	if (!dl->info.exists || !dl->info.live_table) {
		stack;
		r = 0;
		goto out;
	}

	if (_get_flag(dl, RESUME_IMMEDIATE) && dl->info.suspended &&
	    !_resume(dl)) {
		stack;
		r = 0;
		goto out;
	}

	log_very_verbose("Activated %s %s %03u:%03u", dl->name,
			 dl->dlid, dl->info.major, dl->info.minor);

	if (r && _get_flag(dl, VISIBLE))
		fs_add_lv(dl->lv, dl->name);

	_clear_flag(dl, RELOAD);

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _remove(struct dev_layer *dl)
{
	int r;
	struct dm_task *dmt;

	if (_get_flag(dl, VISIBLE))
		log_verbose("Removing %s", dl->name);
	else
		log_very_verbose("Removing %s", dl->name);

	if (!(dmt = _setup_task(dl->name, NULL, 0, DM_DEVICE_REMOVE))) {
		stack;
		return 0;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	/* Suppress error message if it's still in use - we'll log it later */
	log_suppress(1);

	if ((r = dm_task_run(dmt)))
		dl->info.exists = 0;

	log_suppress(0);

	dm_task_destroy(dmt);

	if (r && _get_flag(dl, VISIBLE))
		fs_del_lv(dl->lv);

	_clear_flag(dl, ACTIVE);

	return r;
}

/*
 * The functions that populate the table in a dm_task as part of
 * a create/reload.
 */

/*
 * Emit a target for a given segment.
 * FIXME: tidy this function.
 */
static int _emit_target_line(struct dev_manager *dm, struct dm_task *dmt,
			     struct lv_segment *seg, char *params,
			     size_t paramsize)
{
	uint64_t esize = seg->lv->vg->extent_size;
	int w = 0;
	const char *target = NULL;
	int r;

	if (!seg->segtype->ops->compose_target_line) {
		log_error("_emit_target: Internal error: Can't handle "
			  "segment type %s", seg->segtype->name);
		return 0;
	}

	if ((r = seg->segtype->ops->compose_target_line(dm, dm->mem,
							dm->cmd->cft,
							&dm->target_state, seg,
							params, paramsize,
							&target, &w,
							&dm->
							pvmove_mirror_count)) <=
	    0) {
		stack;
		return r;
	}

	log_debug("Adding target: %" PRIu64 " %" PRIu64 " %s %s",
		  esize * seg->le, esize * seg->len, target, params);

	if (!dm_task_add_target(dmt, esize * seg->le, esize * seg->len,
				target, params)) {
		stack;
		return 0;
	}

	return 1;
}

int compose_log_line(struct dev_manager *dm, struct lv_segment *seg,
		     char *params, size_t paramsize, int *pos, int areas,
		     uint32_t region_size)
{
	int tw;

	tw = lvm_snprintf(params, paramsize, "core 1 %u %u ",
			  region_size, areas);

	if (tw < 0) {
		stack;
		return -1;
	}

	*pos += tw;

	return 1;
}

int compose_areas_line(struct dev_manager *dm, struct lv_segment *seg,
		       char *params, size_t paramsize, int *pos, int start_area,
		       int areas)
{
	uint32_t s;
	int tw = 0;
	const char *trailing_space;
	uint64_t esize = seg->lv->vg->extent_size;
	struct dev_layer *dl;
	char devbuf[10];

	for (s = start_area; s < areas; s++, *pos += tw) {
		trailing_space = (areas - s - 1) ? " " : "";
		if ((seg->area[s].type == AREA_PV &&
		     (!seg->area[s].u.pv.pv || !seg->area[s].u.pv.pv->dev)) ||
		    (seg->area[s].type == AREA_LV && !seg->area[s].u.lv.lv))
			tw = lvm_snprintf(params + *pos, paramsize - *pos,
					  "%s 0%s", dm->stripe_filler,
					  trailing_space);
		else if (seg->area[s].type == AREA_PV)
			tw = lvm_snprintf(params + *pos, paramsize - *pos,
					  "%s %" PRIu64 "%s",
					  dev_name(seg->area[s].u.pv.pv->dev),
					  (seg->area[s].u.pv.pv->pe_start +
					   (esize * seg->area[s].u.pv.pe)),
					  trailing_space);
		else {
			if (!(dl = hash_lookup(dm->layers,
					       seg->area[s].u.lv.lv->lvid.s))) {
				log_error("device layer %s missing from hash",
					  seg->area[s].u.lv.lv->lvid.s);
				return 0;
			}
			if (!dm_format_dev
			    (devbuf, sizeof(devbuf), dl->info.major,
			     dl->info.minor)) {
				log_error
				    ("Failed to format device number as dm target (%u,%u)",
				     dl->info.major, dl->info.minor);
				return 0;
			}
			tw = lvm_snprintf(params + *pos, paramsize - *pos,
					  "%s %" PRIu64 "%s", devbuf,
					  esize * seg->area[s].u.lv.le,
					  trailing_space);
		}

		if (tw < 0) {
			stack;
			return -1;
		}
	}

	return 1;
}

static int _emit_target(struct dev_manager *dm, struct dm_task *dmt,
			struct lv_segment *seg)
{
	char *params;
	size_t paramsize = 4096;
	int ret;

	do {
		if (!(params = dbg_malloc(paramsize))) {
			log_error("Insufficient space for target parameters.");
			return 0;
		}

		ret = _emit_target_line(dm, dmt, seg, params, paramsize);
		dbg_free(params);

		if (!ret)
			stack;

		if (ret >= 0)
			return ret;

		log_debug("Insufficient space in params[%" PRIsize_t
			  "] for target parameters.", paramsize);

		paramsize *= 2;
	} while (paramsize < MAX_TARGET_PARAMSIZE);

	log_error("Target parameter size too big. Aborting.");
	return 0;
}

static int _populate_vanilla(struct dev_manager *dm,
			     struct dm_task *dmt, struct dev_layer *dl)
{
	struct list *segh;
	struct lv_segment *seg;
	struct logical_volume *lv = dl->lv;

	dm->pvmove_mirror_count = 0u;

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct lv_segment);
		if (!_emit_target(dm, dmt, seg)) {
			log_error("Unable to build table for '%s'", lv->name);
			return 0;
		}
	}

	return 1;
}

static int _populate_origin(struct dev_manager *dm,
			    struct dm_task *dmt, struct dev_layer *dl)
{
	char *real;
	char params[PATH_MAX + 32];
	struct dev_layer *dlr;

	if (!(real = _build_dlid(dm->mem, dl->lv->lvid.s, "real"))) {
		stack;
		return 0;
	}

	if (!(dlr = hash_lookup(dm->layers, real))) {
		log_error("Couldn't find real device layer %s in hash", real);
		return 0;
	}

	if (!dm_format_dev(params, sizeof(params), dlr->info.major,
			   dlr->info.minor)) {
		log_error("Couldn't create origin device parameters for '%s'.",
			  real);
		return 0;
	}

	log_debug("Adding target: 0 %" PRIu64 " snapshot-origin %s",
		  dl->lv->size, params);
	if (!dm_task_add_target(dmt, UINT64_C(0), dl->lv->size,
				"snapshot-origin", params)) {
		stack;
		return 0;
	}

	return 1;
}

static int _populate_snapshot(struct dev_manager *dm,
			      struct dm_task *dmt, struct dev_layer *dl)
{
	char *origin, *cow;
	char params[PATH_MAX * 2 + 32];
	struct lv_segment *snap_seg;
	struct dev_layer *dlo, *dlc;
	char devbufo[10], devbufc[10];
	uint64_t size;

	if (!(snap_seg = find_cow(dl->lv))) {
		log_error("Couldn't find snapshot for '%s'.", dl->lv->name);
		return 0;
	}

	if (!(origin = _build_dlid(dm->mem, snap_seg->origin->lvid.s,
				   "real"))) {
		stack;
		return 0;
	}

	if (!(cow = _build_dlid(dm->mem, snap_seg->cow->lvid.s, "cow"))) {
		stack;
		return 0;
	}

	if (!(dlo = hash_lookup(dm->layers, origin))) {
		log_error("Couldn't find origin device layer %s in hash",
			  origin);
		return 0;
	}

	if (!(dlc = hash_lookup(dm->layers, cow))) {
		log_error("Couldn't find cow device layer %s in hash", cow);
		return 0;
	}

	if (!dm_format_dev(devbufo, sizeof(devbufo), dlo->info.major,
			   dlo->info.minor)) {
		log_error("Couldn't create origin device parameters for '%s'.",
			  snap_seg->origin->name);
		return 0;
	}

	if (!dm_format_dev(devbufc, sizeof(devbufc), dlc->info.major,
			   dlc->info.minor)) {
		log_error("Couldn't create cow device parameters for '%s'.",
			  snap_seg->cow->name);
		return 0;
	}

	if (lvm_snprintf(params, sizeof(params), "%s %s P %d",
			 devbufo, devbufc, snap_seg->chunk_size) == -1) {
		stack;
		return 0;
	}

	size = (uint64_t) snap_seg->len * snap_seg->origin->vg->extent_size;

	log_debug("Adding target: 0 %" PRIu64 " snapshot %s", size, params);
	if (!dm_task_add_target(dmt, UINT64_C(0), size, "snapshot", params)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * dev_manager implementation.
 */
struct dev_manager *dev_manager_create(struct cmd_context *cmd,
				       const char *vg_name)
{
	struct pool *mem;
	struct dev_manager *dm;

	if (!(mem = pool_create("dev_manager", 16 * 1024))) {
		stack;
		return NULL;
	}

	if (!(dm = pool_alloc(mem, sizeof(*dm)))) {
		stack;
		goto bad;
	}

	dm->cmd = cmd;
	dm->mem = mem;

	if (!stripe_filler) {
		stripe_filler = find_config_str(cmd->cft->root,
						"activation/missing_stripe_filler",
						DEFAULT_STRIPE_FILLER);
	}
	dm->stripe_filler = stripe_filler;

	if (!(dm->vg_name = pool_strdup(dm->mem, vg_name))) {
		stack;
		goto bad;
	}

	if (!(dm->layers = hash_create(32))) {
		stack;
		goto bad;
	}

	list_init(&dm->active_list);
	list_init(&dm->reload_list);
	list_init(&dm->remove_list);
	list_init(&dm->suspend_list);

	dm->target_state = NULL;

	return dm;

      bad:
	pool_destroy(mem);
	return NULL;
}

void dev_manager_destroy(struct dev_manager *dm)
{
	hash_destroy(dm->layers);
	pool_destroy(dm->mem);
}

int dev_manager_info(struct dev_manager *dm, const struct logical_volume *lv,
		     int mknodes, int with_open_count, struct dm_info *info)
{
	char *name;

	/*
	 * Build a name for the top layer.
	 */
	if (!(name = build_dm_name(dm->mem, lv->vg->name, lv->name, NULL))) {
		stack;
		return 0;
	}

	/*
	 * Try and get some info on this device.
	 */
	log_debug("Getting device info for %s", name);
	if (!_info(name, lv->lvid.s, mknodes, with_open_count, info, NULL,
		   NULL)) {
		stack;
		return 0;
	}

	return 1;
}

int dev_manager_snapshot_percent(struct dev_manager *dm,
				 struct logical_volume *lv, float *percent)
{
	char *name;

	/*
	 * Build a name for the top layer.
	 */
	if (!(name = build_dm_name(dm->mem, lv->vg->name, lv->name, NULL))) {
		stack;
		return 0;
	}

	/*
	 * Try and get some info on this device.
	 */
	log_debug("Getting device status percentage for %s", name);
	if (!(_percent(dm, name, lv->lvid.s, "snapshot", 0, NULL, percent,
		       NULL))) {
		stack;
		return 0;
	}

	/* FIXME pool_free ? */

	/* If the snapshot isn't available, percent will be -1 */
	return 1;
}

/* FIXME Merge with snapshot_percent, auto-detecting target type */
/* FIXME Cope with more than one target */
int dev_manager_mirror_percent(struct dev_manager *dm,
			       struct logical_volume *lv, int wait,
			       float *percent, uint32_t *event_nr)
{
	char *name;

	/*
	 * Build a name for the top layer.
	 */
	if (!(name = build_dm_name(dm->mem, lv->vg->name, lv->name, NULL))) {
		stack;
		return 0;
	}

	/* FIXME pool_free ? */

	log_debug("Getting device mirror status percentage for %s", name);
	if (!(_percent(dm, name, lv->lvid.s, "mirror", wait, lv, percent,
		       event_nr))) {
		stack;
		return 0;
	}

	return 1;
}

static struct dev_layer *_create_dev(struct dev_manager *dm, char *name,
				     const char *dlid)
{
	struct dev_layer *dl;
	char *uuid;

	if (!(dl = pool_zalloc(dm->mem, sizeof(*dl)))) {
		stack;
		return NULL;
	}

	dl->name = name;

	log_debug("Getting device info for %s", dl->name);
	if (!_info(dl->name, dlid, 0, 0, &dl->info, dm->mem, &uuid)) {
		stack;
		return NULL;
	}

	if (dl->info.exists)
		dl->dlid = uuid;
	else
		dl->dlid = dlid;

	list_init(&dl->pre_create);
	list_init(&dl->pre_suspend);

	if (!hash_insert(dm->layers, dl->dlid, dl)) {
		stack;
		return NULL;
	}

	return dl;
}

static inline int _read_only_lv(struct logical_volume *lv)
{
	return (!(lv->vg->status & LVM_WRITE) || !(lv->status & LVM_WRITE));
}

static struct dev_layer *_create_layer(struct dev_manager *dm,
				       const char *layer,
				       struct logical_volume *lv)
{
	char *name, *dlid;
	struct dev_layer *dl;

	if (!(name = build_dm_name(dm->mem, lv->vg->name, lv->name, layer))) {
		stack;
		return NULL;
	}

	if (!(dlid = _build_dlid(dm->mem, lv->lvid.s, layer))) {
		stack;
		return NULL;
	}

	if (!(dl = hash_lookup(dm->layers, dlid)) &&
	    !(dl = _create_dev(dm, name, dlid))) {
		stack;
		return NULL;
	}

	dl->lv = lv;

	if (!_read_only_lv(lv))
		_set_flag(dl, READWRITE);

	return dl;
}

/*
 * Finds the specified layer.
 */
static struct dev_layer *_lookup(struct dev_manager *dm,
				 const char *lvid, const char *layer)
{
	char *dlid;
	struct dev_layer *dl;

	if (!(dlid = _build_dlid(dm->mem, lvid, layer))) {
		stack;
		return NULL;
	}

	dl = hash_lookup(dm->layers, dlid);
	pool_free(dm->mem, dlid);
	return dl;
}

static int _expand_vanilla(struct dev_manager *dm, struct logical_volume *lv,
			   int was_origin)
{
	/*
	 * only one layer.
	 */
	struct dev_layer *dl, *dlr;
	struct list *segh;
	struct lv_segment *seg;
	uint32_t s;

	if (!(dl = _create_layer(dm, NULL, lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_vanilla;
	if (lv->status & VISIBLE_LV) {
		_set_flag(dl, VISIBLE);
		_set_flag(dl, TOPLEVEL);
	}

	if (lv->status & PVMOVE)
		_set_flag(dl, TOPLEVEL);

	/* Add dependencies for any LVs that segments refer to */
	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct lv_segment);
		for (s = 0; s < seg->area_count; s++) {
			if (seg->area[s].type != AREA_LV)
				continue;
			if (!str_list_add(dm->mem, &dl->pre_create,
					  _build_dlid(dm->mem,
						      seg->area[s].u.lv.lv->
						      lvid.s, NULL))) {
				stack;
				return 0;
			}
			_set_flag(dl, NOPROPAGATE);
		}
	}

	if (!was_origin)
		return 1;

	/* Deactivating the last snapshot */
	if (!(dlr = _create_layer(dm, "real", lv))) {
		stack;
		return 0;
	}

	dlr->populate = _populate_vanilla;
	_clear_flag(dlr, VISIBLE);
	_clear_flag(dlr, TOPLEVEL);
	_set_flag(dlr, REMOVE);

	/* add the dependency on the real device */
	if (!str_list_add(dm->mem, &dl->pre_create,
			  pool_strdup(dm->mem, dlr->dlid))) {
		stack;
		return 0;
	}

	return 1;
}

static int _expand_origin_real(struct dev_manager *dm,
			       struct logical_volume *lv)
{
	struct dev_layer *dl;
	const char *real_dlid;

	if (!(dl = _create_layer(dm, "real", lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_vanilla;
	_clear_flag(dl, VISIBLE);
	_clear_flag(dl, TOPLEVEL);

	/* Size changes must take effect before tables using it are reloaded */
	_set_flag(dl, RESUME_IMMEDIATE);

	real_dlid = dl->dlid;

	if (!(dl = _create_layer(dm, NULL, lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_origin;
	_set_flag(dl, VISIBLE);
	_set_flag(dl, TOPLEVEL);

	/* add the dependency on the real device */
	if (!str_list_add(dm->mem, &dl->pre_create,
			  pool_strdup(dm->mem, real_dlid))) {
		stack;
		return 0;
	}

	return 1;
}

static int _expand_origin(struct dev_manager *dm, struct logical_volume *lv)
{
	struct logical_volume *active;
	struct lv_segment *snap_seg;
	struct list *sh;

	/*
	 * We only need to create an origin layer if one of our
	 * snapshots is in the active list
	 */
	list_iterate(sh, &dm->active_list) {
		active = list_item(sh, struct lv_list)->lv;
		if ((snap_seg = find_cow(active)) && (snap_seg->origin == lv))
			return _expand_origin_real(dm, lv);
	}

	/*
	 * We're deactivating the last snapshot
	 */
	return _expand_vanilla(dm, lv, 1);
}

static int _expand_snapshot(struct dev_manager *dm, struct logical_volume *lv,
			    struct lv_segment *snap_seg)
{
	/*
	 * snapshot(org, cow)
	 * cow
	 */
	struct dev_layer *dl;
	const char *cow_dlid;

	if (!(dl = _create_layer(dm, "cow", lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_vanilla;
	_clear_flag(dl, VISIBLE);
	_clear_flag(dl, TOPLEVEL);
	_set_flag(dl, READWRITE);

	cow_dlid = dl->dlid;

	if (!(dl = _create_layer(dm, NULL, lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_snapshot;
	_set_flag(dl, VISIBLE);
	_set_flag(dl, TOPLEVEL);

	/* add the dependency on the cow device */
	if (!str_list_add(dm->mem, &dl->pre_create,
			  pool_strdup(dm->mem, cow_dlid))) {
		stack;
		return 0;
	}

	/* add the dependency on the real origin device */
	if (!str_list_add(dm->mem, &dl->pre_create,
			  _build_dlid(dm->mem, snap_seg->origin->lvid.s,
				      "real"))) {
		stack;
		return 0;
	}

	/* add the dependency on the visible origin device */
	if (!str_list_add(dm->mem, &dl->pre_suspend,
			  snap_seg->origin->lvid.s)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Inserts the appropriate dev_layers for a logical volume.
 */
static int _expand_lv(struct dev_manager *dm, struct logical_volume *lv)
{
	struct lv_segment *snap_seg;

	/*
	 * FIXME: this doesn't cope with recursive snapshots yet.
	 */
	if ((snap_seg = find_cow(lv)))
		return _expand_snapshot(dm, lv, snap_seg);

	else if (lv_is_origin(lv))
		return _expand_origin(dm, lv);

	return _expand_vanilla(dm, lv, 0);
}

/*
 * Clears the mark bit on all layers.
 */
static void _clear_marks(struct dev_manager *dm, int flag)
{
	struct hash_node *hn;
	struct dev_layer *dl;

	hash_iterate(hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);
		_clear_flag(dl, flag);
	}
}

/*
 * Propogates marks via the pre_create dependency list.
 */
static int _trace_layer_marks(struct dev_manager *dm, struct dev_layer *dl,
			      int flag)
{
	struct list *sh;
	const char *dlid;
	struct dev_layer *dep;

	list_iterate(sh, &dl->pre_create) {
		dlid = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, dlid))) {
			log_error("Couldn't find device layer '%s'.", dlid);
			return 0;
		}

		if (_get_flag(dep, flag))
			continue;

		/* FIXME Only propagate LV ACTIVE dependencies for now */
		if ((flag != ACTIVE) && _get_flag(dl, NOPROPAGATE))
			continue;

		_set_flag(dep, flag);

		if (!_trace_layer_marks(dm, dep, flag)) {
			stack;
			return 0;
		}
	}

	return 1;
}

/*
 * Calls _trace_single for every marked layer.
 */
static int _trace_all_marks(struct dev_manager *dm, int flag)
{
	struct hash_node *hn;
	struct dev_layer *dl;

	hash_iterate(hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);
		if (_get_flag(dl, flag) && !_trace_layer_marks(dm, dl, flag)) {
			stack;
			return 0;
		}
	}

	return 1;
}

/*
 * Marks the top layers, then traces these through the
 * dependencies.
 */
static int _mark_lvs(struct dev_manager *dm, struct list *lvs, int flag)
{
	struct list *lvh;
	struct logical_volume *lv;
	struct dev_layer *dl;

	list_iterate(lvh, lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		if (lv->status & SNAPSHOT)
			continue;

		if (!(dl = _lookup(dm, lv->lvid.s, NULL))) {
			stack;
			return 0;
		}

		_set_flag(dl, flag);
	}

	if (!_trace_all_marks(dm, flag)) {
		stack;
		return 0;
	}

	return 1;
}

static int _suspend_parents(struct dev_manager *dm, struct dev_layer *dl)
{
	struct list *sh;
	struct dev_layer *dep;
	const char *dlid;

	list_iterate(sh, &dl->pre_suspend) {
		dlid = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, dlid))) {
			log_debug("_suspend_parents couldn't find device "
				  "layer '%s' - skipping.", dlid);
			continue;
		}

		if (!strcmp(dep->dlid, dl->dlid)) {
			log_error("BUG: pre-suspend loop detected (%s)", dlid);
			return 0;
		}

		if (!_suspend_parents(dm, dep)) {
			stack;
			return 0;
		}

		if (dep->info.exists & !_suspend(dep)) {
			stack;
			return 0;
		}
	}

	return 1;
}

static int _resume_with_deps(struct dev_manager *dm, struct dev_layer *dl)
{
	struct list *sh;
	struct dev_layer *dep;
	const char *dlid;

	list_iterate(sh, &dl->pre_create) {
		dlid = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, dlid))) {
			log_debug("_resume_with_deps couldn't find device "
				  "layer '%s' - skipping.", dlid);
			continue;
		}

		if (!strcmp(dep->dlid, dl->dlid)) {
			log_error("BUG: pre-create loop detected (%s)", dlid);
			return 0;
		}

		if (!_resume_with_deps(dm, dep)) {
			stack;
			return 0;
		}
	}

	if (dl->info.exists & !_get_flag(dl, SUSPENDED) && !_resume(dl)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * Recurses through the tree, ensuring that devices are created
 * in correct order.
 */
static int _create_rec(struct dev_manager *dm, struct dev_layer *dl)
{
	struct list *sh;
	struct dev_layer *dep;
	const char *dlid;
	char *newname, *suffix;

	/* Suspend? */
	if (_get_flag(dl, SUSPENDED) &&
	    (!_suspend_parents(dm, dl) || !_suspend(dl))) {
		stack;
		return 0;
	}

	list_iterate(sh, &dl->pre_create) {
		dlid = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, dlid))) {
			log_error("Couldn't find device layer '%s'.", dlid);
			return 0;
		}

		if (!strcmp(dep->dlid, dl->dlid)) {
			log_error("BUG: pre-create loop detected (%s)", dlid);
			return 0;
		}

		if (!_create_rec(dm, dep)) {
			stack;
			return 0;
		}
	}

	/* Rename? */
	if (dl->info.exists) {
		if ((suffix = rindex(dl->dlid, '-')))
			suffix++;
		newname = build_dm_name(dm->mem, dm->vg_name, dl->lv->name,
					suffix);
		if (strcmp(newname, dl->name)) {
			if (!_suspend_parents(dm, dl) ||
			    !_suspend(dl) || !_rename(dm, dl, newname)) {
				stack;
				return 0;
			}
		}
	}

	/* Create? */
	if (!dl->info.exists) {
		if (!_suspend_parents(dm, dl) ||
		    !_load(dm, dl, DM_DEVICE_CREATE)) {
			stack;
			return 0;
		}
		return 1;
	}

	/* Reload? */
	if (_get_flag(dl, RELOAD) &&
	    (!_suspend_parents(dm, dl) || !_suspend(dl) ||
	     !_load(dm, dl, DM_DEVICE_RELOAD))) {
		stack;
		return 0;
	}

	return 1;
}

static int _build_all_layers(struct dev_manager *dm, struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;

	/*
	 * Build layers for complete vg.
	 */
	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		if (lv->status & SNAPSHOT)
			continue;
		if (!_expand_lv(dm, lv)) {
			stack;
			return 0;
		}
	}

	return 1;
}

static int _fill_in_remove_list(struct dev_manager *dm)
{
	struct hash_node *hn;
	struct dev_layer *dl;
	struct dl_list *dll;

	hash_iterate(hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);

		if (_get_flag(dl, REMOVE))
			_clear_flag(dl, ACTIVE);

		if (!_get_flag(dl, ACTIVE)) {
			dll = pool_alloc(dm->mem, sizeof(*dll));
			if (!dll) {
				stack;
				return 0;
			}

			dll->dl = dl;
			list_add(&dm->remove_list, &dll->list);
		}
	}

	return 1;
}

static int _populate_pre_suspend_lists(struct dev_manager *dm)
{
	struct hash_node *hn;
	struct dev_layer *dl;
	struct list *sh;
	const char *dlid;
	struct dev_layer *dep;

	hash_iterate(hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);

		list_iterate(sh, &dl->pre_suspend) {
			dlid = list_item(sh, struct str_list)->str;

			if (!(dep = hash_lookup(dm->layers, dlid))) {
				log_debug("_populate_pre_suspend_lists: "
					  "Couldn't find device layer '%s' - "
					  "skipping.", dlid);
				continue;
			}

			if (!str_list_add(dm->mem, &dep->pre_create, dl->dlid)) {
				stack;
				return 0;
			}
		}

		list_iterate(sh, &dl->pre_create) {
			dlid = list_item(sh, struct str_list)->str;

			if (!(dep = hash_lookup(dm->layers, dlid))) {
				log_debug("_populate_pre_suspend_lists: "
					  "Couldn't find device layer '%s' - "
					  "skipping.", dlid);
				continue;
			}

			if (!str_list_add(dm->mem, &dep->pre_suspend, dl->dlid)) {
				stack;
				return 0;
			}
		}
	}

	return 1;
}

/*
 * Layers are removed in a top-down manner.
 */
static int _remove_old_layers(struct dev_manager *dm)
{
	int change;
	struct list *rh, *n;
	struct dev_layer *dl;

	do {
		change = 0;
		list_iterate_safe(rh, n, &dm->remove_list) {
			dl = list_item(rh, struct dl_list)->dl;

			if (!dl->info.exists) {
				list_del(rh);
				continue;
			}

			if (_remove(dl)) {
				change = 1;
				list_del(rh);
			}
		}

	} while (change);

	if (!list_empty(&dm->remove_list)) {
		list_iterate(rh, &dm->remove_list) {
			dl = list_item(rh, struct dl_list)->dl;
			log_error("Couldn't deactivate device %s", dl->name);
		}
		return 0;
	}

	return 1;
}

/*
 * The guts of the activation unit, this examines the device
 * layers in the manager, and tries to issue the correct
 * instructions to activate them in order.
 */
static int _execute(struct dev_manager *dm, struct volume_group *vg)
{
	struct hash_node *hn;
	struct dev_layer *dl;

	if (!_build_all_layers(dm, vg)) {
		stack;
		return 0;
	}

	/*
	 * Mark all layer that need reloading.
	 */
	_clear_marks(dm, RELOAD);
	if (!_mark_lvs(dm, &dm->reload_list, RELOAD)) {
		stack;
		return 0;
	}

	/*
	 * Mark all layers that should be active.
	 */
	_clear_marks(dm, ACTIVE);
	if (!_mark_lvs(dm, &dm->active_list, ACTIVE)) {
		stack;
		return 0;
	}

	/* 
	 * Mark all layers that should be left suspended.
	 */
	_clear_marks(dm, SUSPENDED);
	if (!_mark_lvs(dm, &dm->suspend_list, SUSPENDED)) {
		stack;
		return 0;
	}

	if (!_fill_in_remove_list(dm)) {
		stack;
		return 0;
	}

	if (!_populate_pre_suspend_lists(dm)) {
		stack;
		return 0;
	}

	/*
	 * Now only top level devices will be unmarked.
	 */
	hash_iterate(hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);

		if (_get_flag(dl, ACTIVE) && _get_flag(dl, TOPLEVEL))
			if (!_create_rec(dm, dl)) {
				stack;
				return 0;
			}
	}

	/* Resume devices */
	hash_iterate(hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);

		if (!_resume_with_deps(dm, dl)) {
			stack;
			return 0;
		}
	}

	if (!_remove_old_layers(dm)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * ATM we decide which vg a layer belongs to by
 * looking at the beginning of the device
 * name.
 */
static int _belong_to_vg(const char *vgname, const char *name)
{
	const char *v = vgname, *n = name;

	while (*v) {
		if ((*v != *n) || (*v == '-' && *(++n) != '-'))
			return 0;
		v++, n++;
	}

	if (*n == '-' && *(n + 1) != '-')
		return 1;
	else
		return 0;
}

static int _add_existing_layer(struct dev_manager *dm, const char *name)
{
	struct dev_layer *dl;
	char *copy;

	log_debug("Found existing layer '%s'", name);

	if (!(copy = pool_strdup(dm->mem, name))) {
		stack;
		return 0;
	}

	if (!(dl = _create_dev(dm, copy, ""))) {
		stack;
		return 0;
	}

	return 1;
}

static int _scan_existing_devices(struct dev_manager *dm)
{
	int r = 0;
	struct dm_names *names;
	unsigned next = 0;

	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_LIST)))
		return 0;

	if (!dm_task_run(dmt))
		goto out;

	if (!(names = dm_task_get_names(dmt)))
		goto out;

	r = 1;
	if (!names->dev)
		goto out;

	do {
		names = (void *) names + next;
		if (_belong_to_vg(dm->vg_name, names->name) &&
		    !_add_existing_layer(dm, names->name)) {
			stack;
			r = 0;
			break;
		}
		next = names->next;
	} while (next);

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _add_lv(struct pool *mem,
		   struct list *head, struct logical_volume *lv)
{
	struct lv_list *lvl;

	if (!(lvl = pool_alloc(mem, sizeof(*lvl)))) {
		stack;
		return 0;
	}

	lvl->lv = lv;
	list_add(head, &lvl->list);

	return 1;
}

static int _add_lvs(struct pool *mem,
		    struct list *head, struct logical_volume *origin)
{
	struct logical_volume *lv;
	struct lv_segment *snap_seg;
	struct list *lvh;

	list_iterate(lvh, &origin->vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		if (lv->status & SNAPSHOT)
			continue;
		if ((snap_seg = find_cow(lv)) && snap_seg->origin == origin)
			if (!_add_lv(mem, head, lv))
				return 0;
	}

	return _add_lv(mem, head, origin);
}

static void _remove_lv(struct list *head, struct logical_volume *lv)
{
	struct list *lvh;
	struct lv_list *lvl;

	list_iterate(lvh, head) {
		lvl = list_item(lvh, struct lv_list);
		if (lvl->lv == lv) {
			list_del(lvh);
			break;
		}
	}
}

static int _remove_lvs(struct dev_manager *dm, struct logical_volume *lv)
{
	struct logical_volume *active, *old_origin;
	struct lv_segment *snap_seg;
	struct list *sh, *active_head;

	active_head = &dm->active_list;

	/* Remove any snapshots with given origin */
	list_iterate(sh, active_head) {
		active = list_item(sh, struct lv_list)->lv;
		if ((snap_seg = find_cow(active)) && snap_seg->origin == lv) {
			_remove_lv(active_head, active);
		}
	}

	_remove_lv(active_head, lv);

	if (!(snap_seg = find_cow(lv)))
		return 1;

	old_origin = snap_seg->origin;

	/* Was this the last active snapshot with this origin? */
	list_iterate(sh, active_head) {
		active = list_item(sh, struct lv_list)->lv;
		if ((snap_seg = find_cow(active)) &&
		    snap_seg->origin == old_origin) {
			return 1;
		}
	}

	return _add_lvs(dm->mem, &dm->reload_list, old_origin);
}

static int _remove_suspended_lvs(struct dev_manager *dm,
				 struct logical_volume *lv)
{
	struct logical_volume *suspended;
	struct lv_segment *snap_seg;
	struct list *sh, *suspend_head;

	suspend_head = &dm->suspend_list;

	/* Remove from list any snapshots with given origin */
	list_iterate(sh, suspend_head) {
		suspended = list_item(sh, struct lv_list)->lv;
		if ((snap_seg = find_cow(suspended)) &&
		    snap_seg->origin == lv) {
			_remove_lv(suspend_head, suspended);
		}
	}

	_remove_lv(suspend_head, lv);

	return 1;
}

static int _targets_present(struct dev_manager *dm, struct list *lvs)
{
	struct logical_volume *lv;
	struct list *lvh, *segh;
	struct segment_type *segtype;
	struct lv_segment *seg;
	int snapshots = 0, mirrors = 0;

	list_iterate(lvh, lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		if (!snapshots)
			if (lv_is_cow(lv) || lv_is_origin(lv))
				snapshots = 1;

		if (!mirrors)
			if (lv->status & PVMOVE)
				mirrors = 1;

		if (lv->status & VIRTUAL) {
			list_iterate(segh, &lv->segments) {
				seg = list_item(segh, struct lv_segment);
				if (seg->segtype->ops->target_present &&
				    !seg->segtype->ops->target_present()) {
					log_error("Can't expand LV: %s target "
						  "support missing "
						  "from kernel?",
						  seg->segtype->name);
					return 0;
				}
			}
		}
	}

	if (mirrors) {
		if (!(segtype = get_segtype_from_string(dm->cmd, "mirror"))) {
			log_error("Can't expand LV: Mirror support "
				  "missing from tools?");
			return 0;
		}

		if (!segtype->ops->target_present ||
		    !segtype->ops->target_present()) {
			log_error("Can't expand LV: Mirror support missing "
				  "from kernel?");
			return 0;
		}
	}

	if (snapshots) {
		if (!(segtype = get_segtype_from_string(dm->cmd, "snapshot"))) {
			log_error("Can't expand LV: Snapshot support "
				  "missing from tools?");
			return 0;
		}

		if (!segtype->ops->target_present ||
		    !segtype->ops->target_present()) {
			log_error("Can't expand LV: Snapshot support missing "
				  "from kernel?");
			return 0;
		}
	}

	return 1;
}

static int _fill_in_active_list(struct dev_manager *dm, struct volume_group *vg)
{
	char *dlid;
	struct list *lvh;
	struct logical_volume *lv;
	struct dev_layer *dl;

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		if (lv->status & SNAPSHOT)
			continue;

		if (!(dlid = _build_dlid(dm->mem, lv->lvid.s, NULL))) {
			stack;
			return 0;
		}

		dl = hash_lookup(dm->layers, dlid);
		pool_free(dm->mem, dlid);

		if (dl) {
			log_debug("Found active lv %s%s", lv->name,
				  dl->info.suspended ? " (suspended)" : "");

			if (!_add_lv(dm->mem, &dm->active_list, lv)) {
				stack;
				return 0;
			}

			if (dl->info.suspended) {
				if (!_add_lv(dm->mem, &dm->suspend_list, lv)) {
					stack;
					return 0;
				}
			}
		}
	}

	return 1;
}

static int _action(struct dev_manager *dm, struct logical_volume *lv,
		   action_t action)
{
	if (!_scan_existing_devices(dm)) {
		stack;
		return 0;
	}

	if (!_fill_in_active_list(dm, lv->vg)) {
		stack;
		return 0;
	}

	if (action == ACTIVATE || action == DEACTIVATE)
		/* Get into known state - remove from active list if present */
		if (!_remove_lvs(dm, lv)) {
			stack;
			return 0;
		}

	if (action == ACTIVATE) {
		/* Add to active & reload lists */
		if (!_add_lvs(dm->mem, &dm->reload_list, lv) ||
		    !_add_lvs(dm->mem, &dm->active_list, lv)) {
			stack;
			return 0;
		}
	}

	if (action == SUSPEND || action == RESUME || action == ACTIVATE)
		/* Get into known state - remove from suspend list if present */
		if (!_remove_suspended_lvs(dm, lv)) {
			stack;
			return 0;
		}

	if (action == SUSPEND) {
		if (!_add_lvs(dm->mem, &dm->suspend_list, lv)) {
			stack;
			return 0;
		}
	}

	if (!_targets_present(dm, &dm->active_list) ||
	    !_targets_present(dm, &dm->reload_list)) {
		stack;
		return 0;
	}

	if (!_execute(dm, lv->vg)) {
		stack;
		return 0;
	}

	return 1;
}

int dev_manager_activate(struct dev_manager *dm, struct logical_volume *lv)
{
	return _action(dm, lv, ACTIVATE);
}

int dev_manager_deactivate(struct dev_manager *dm, struct logical_volume *lv)
{
	return _action(dm, lv, DEACTIVATE);
}

int dev_manager_suspend(struct dev_manager *dm, struct logical_volume *lv)
{
	return _action(dm, lv, SUSPEND);
}

int dev_manager_lv_mknodes(const struct logical_volume *lv)
{
	char *name;

	if (!(name = build_dm_name(lv->vg->cmd->mem, lv->vg->name,
				   lv->name, NULL))) {
		stack;
		return 0;
	}

	return fs_add_lv(lv, name);
}

int dev_manager_lv_rmnodes(const struct logical_volume *lv)
{
	return fs_del_lv(lv);
}

int dev_manager_mknodes(void)
{
	struct dm_task *dmt;
	int r;

	if (!(dmt = dm_task_create(DM_DEVICE_MKNODES)))
		return 0;

	r = dm_task_run(dmt);

	dm_task_destroy(dmt);
	return r;
}

void dev_manager_exit(void)
{
	dm_lib_exit();
}
