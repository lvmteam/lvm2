/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "dev_manager.h"
#include "pool.h"
#include "hash.h"
#include "log.h"
#include "lvm-string.h"
#include "fs.h"

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

enum {
	ACTIVE = 0,
	RELOAD = 1,
	VISIBLE = 2,
	READWRITE = 3,
	SUSPENDED = 4
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
	char *dlid;

	struct logical_volume *lv;

	/*
	 * Devices that must be created before this one can be
	 * created.  Holds str_lists.
	 */
	struct list pre_create;

};

struct dl_list {
	struct list list;
	struct dev_layer *dl;
};

struct dev_manager {
	struct pool *mem;

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

static int _pre_list_add(struct pool *mem, struct list *pl, char *str)
{
	struct str_list *sl;
	struct list *plh;

	if (!str) {
		stack;
		return 0;
	}

	list_iterate(plh, pl) {
	}

	if (!(sl = pool_alloc(mem, sizeof(*sl)))) {
		stack;
		return 0;
	}

	sl->str = str;
	list_add(pl, &sl->list);

	return 1;
}

/*
 * Device layer names are all of the form <vg>-<lv>-<layer>, any
 * other hyphens that appear in these names are quoted with yet
 * another hyphen.  The top layer of any device has no layer
 * name.  eg, vg0-lvol0.
 */
static void _count_hyphens(const char *str, size_t * len, int *hyphens)
{
	const char *ptr;

	for (ptr = str; *ptr; ptr++, (*len)++)
		if (*ptr == '-')
			(*hyphens)++;
}

/*
 * Copies a string, quoting hyphens with hyphens.
 */
static void _quote_hyphens(char **out, const char *src)
{
	while (*src) {
		if (*src == '-')
			*(*out)++ = '-';

		*(*out)++ = *src++;
	}
}

/*
 * <vg>-<lv>-<layer> or if !layer just <vg>-<lv>.
 */
static char *_build_name(struct pool *mem, const char *vg,
			 const char *lv, const char *layer)
{
	size_t len = 0;
	int hyphens = 0;
	char *r, *out;

	_count_hyphens(vg, &len, &hyphens);
	_count_hyphens(lv, &len, &hyphens);

	if (layer && *layer)
		_count_hyphens(layer, &len, &hyphens);

	len += hyphens + 2;

	if (!(r = pool_alloc(mem, len))) {
		stack;
		return NULL;
	}

	out = r;
	_quote_hyphens(&out, vg);
	*out++ = '-';
	_quote_hyphens(&out, lv);

	if (layer && *layer) {
		*out++ = '-';
		_quote_hyphens(&out, layer);
	}
	*out = '\0';

	return r;
}

/* Find start of LV component in hyphenated name */
static char *_find_lv_name(char *vg)
{
	char *c = vg;

	while (*c && *(c + 1)) {
		if (*c == '-') {
			if (*(c + 1) == '-')
				c++;
			else
				return (c + 1);
		}
		c++;
	}

	return NULL;
}

static char *_build_dlid(struct pool *mem, const char *lvid, const char *layer)
{
	char *dlid;
	int len;

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
static struct dm_task *_setup_task(const char *name, const char *uuid, int task)
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

	return dmt;
}

static int _info_run(const char *name, const char *uuid, struct dm_info *info,
		     struct pool *mem, char **uuid_out)
{
	int r = 0;
	struct dm_task *dmt;
	const char *u;

	if (!(dmt = _setup_task(name, uuid, DM_DEVICE_INFO))) {
		stack;
		return 0;
	}

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

static int _info(const char *name, const char *uuid, struct dm_info *info,
		 struct pool *mem, char **uuid_out)
{
	if (uuid && *uuid && _info_run(NULL, uuid, info, mem, uuid_out)
	    && info->exists)
		return 1;

	if (name)
		return _info_run(name, NULL, info, mem, uuid_out);

	return 0;
}

static int _rename(struct dev_manager *dm, struct dev_layer *dl, char *newname)
{
	int r = 1;
	struct dm_task *dmt;

	log_verbose("Renaming %s to %s", dl->name, newname);

	if (!(dmt = _setup_task(dl->name, NULL, DM_DEVICE_RENAME))) {
		stack;
		return 0;
	}

	if (!dm_task_set_newname(dmt, newname)) {
		stack;
		r = 0;
		goto out;
	}

	if (!(r = dm_task_run(dmt)))
		log_error("Couldn't rename device '%s'.", dl->name);

	if (r && _get_flag(dl, VISIBLE))
		fs_rename_lv(dl->lv, newname, _find_lv_name(dl->name));

	dl->name = newname;

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _load(struct dev_manager *dm, struct dev_layer *dl, int task)
{
	int r = 1;
	struct dm_task *dmt;

	log_verbose("Loading %s", dl->name);
	if (!(dmt = _setup_task(task == DM_DEVICE_CREATE ? dl->name : NULL,
				dl->dlid, task))) {
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
	 * Do we want a specific minor number ?
	 */
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

	if (!(r = dm_task_run(dmt)))
		log_error("Couldn't load device '%s'.", dl->name);

	if (!dm_task_get_info(dmt, &dl->info)) {
		stack;
		r = 0;
		goto out;
	}

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

	if (!(dmt = _setup_task(dl->name, NULL, DM_DEVICE_REMOVE))) {
		stack;
		return 0;
	}

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

static int _suspend_or_resume(const char *name, action_t suspend)
{
	int r;
	struct dm_task *dmt;
	int sus = (suspend == SUSPEND) ? 1 : 0;
	int task = sus ? DM_DEVICE_SUSPEND : DM_DEVICE_RESUME;

	log_very_verbose("%s %s", sus ? "Suspending" : "Resuming", name);
	if (!(dmt = _setup_task(name, NULL, task))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_error("Couldn't %s device '%s'", sus ? "suspend" : "resume",
			  name);

	dm_task_destroy(dmt);
	return r;
}

static int _suspend(struct dev_layer *dl)
{
	if (dl->info.suspended)
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
	if (!dl->info.suspended)
		return 1;

	if (!_suspend_or_resume(dl->name, RESUME)) {
		stack;
		return 0;
	}

	dl->info.suspended = 0;
	return 1;
}

/*
 * The functions that populate the table in a dm_task as part of
 * a create/reload.
 */

/*
 * Emit a target for a given segment.
 * FIXME: tidy this function.
 */
static int _emit_target(struct dm_task *dmt, struct stripe_segment *seg)
{
	char params[1024];
	uint64_t esize = seg->lv->vg->extent_size;
	uint32_t s, stripes = seg->stripes;
	int w = 0, tw = 0;
	char *filler = "/dev/ioerror";
	char *target;

	if (stripes == 1) {
		if (!seg->area[0].pv) {
			target = "error";
			goto add_target;
		} else
			target = "linear";
	} else if (stripes > 1) {
		target = "striped";
		if ((tw = lvm_snprintf(params, sizeof(params), "%u %u ",
				       stripes, seg->stripe_size)) < 0)
			goto error;
		w = tw;
	}

	for (s = 0; s < stripes; s++, w += tw) {
		if (!seg->area[s].pv)
			tw = lvm_snprintf(params + w, sizeof(params) - w,
					  "%s 0%s", filler,
					  s == (stripes - 1) ? "" : " ");
		else
			tw = lvm_snprintf(params + w, sizeof(params) - w,
					 "%s %" PRIu64 "%s",
					 dev_name(seg->area[s].pv->dev),
					 (seg->area[s].pv->pe_start +
					  (esize * seg->area[s].pe)),
					 s == (stripes - 1) ? "" : " ");

		if (tw < 0)
			goto error;
	}

      add_target:
	log_debug("Adding target: %" PRIu64 " %" PRIu64 " %s %s",
		  esize * seg->le, esize * seg->len, target, params);

	if (!dm_task_add_target(dmt, esize * seg->le, esize * seg->len,
				target, params)) {
		stack;
		return 0;
	}

	return 1;

      error:
	log_error("Insufficient space to write target parameters.");
	return 0;
}

static int _populate_vanilla(struct dev_manager *dm,
			     struct dm_task *dmt, struct dev_layer *dl)
{
	struct list *segh;
	struct stripe_segment *seg;
	struct logical_volume *lv = dl->lv;

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct stripe_segment);
		if (!_emit_target(dmt, seg)) {
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

	if (!(real = _build_name(dm->mem, dm->vg_name, dl->lv->name, "real"))) {
		stack;
		return 0;
	}

	if (lvm_snprintf(params, sizeof(params), "%s/%s", dm_dir(), real) == -1) {
		log_error("Couldn't create origin device parameters for '%s'.",
			  real);
		return 0;
	}

	log_debug("Adding target: 0 %" PRIu64 " snapshot-origin %s",
		  dl->lv->size, params);
	if (!dm_task_add_target(dmt, 0, dl->lv->size,
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
	struct snapshot *s;

	if (!(s = find_cow(dl->lv))) {
		log_error("Couldn't find snapshot for '%s'.", dl->lv->name);
		return 0;
	}

	if (!(origin = _build_name(dm->mem, dm->vg_name,
				   s->origin->name, "real"))) {
		stack;
		return 0;
	}

	if (!(cow = _build_name(dm->mem, dm->vg_name, s->cow->name, "cow"))) {
		stack;
		return 0;
	}

	if (snprintf(params, sizeof(params), "%s/%s %s/%s P %d 128",
		     dm_dir(), origin, dm_dir(), cow, s->chunk_size) == -1) {
		stack;
		return 0;
	}

	log_debug("Adding target: 0 %" PRIu64 " snapshot %s",
		  s->origin->size, params);
	if (!dm_task_add_target(dmt, 0, s->origin->size, "snapshot", params)) {
		stack;
		return 0;
	}

	return 1;
}

/*
 * dev_manager implementation.
 */
struct dev_manager *dev_manager_create(const char *vg_name)
{
	struct pool *mem;
	struct dev_manager *dm;

	if (!(mem = pool_create(16 * 1024))) {
		stack;
		return NULL;
	}

	if (!(dm = pool_alloc(mem, sizeof(*dm)))) {
		stack;
		goto bad;
	}

	dm->mem = mem;

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

int dev_manager_info(struct dev_manager *dm, struct logical_volume *lv,
		     struct dm_info *info)
{
	char *name;

	/*
	 * Build a name for the top layer.
	 */
	if (!(name = _build_name(dm->mem, lv->vg->name, lv->name, NULL))) {
		stack;
		return 0;
	}

	/*
	 * Try and get some info on this device.
	 */
	log_debug("Getting device info for %s", name);
	if (!_info(name, lv->lvid.s, info, NULL, NULL)) {
		stack;
		return 0;
	}

	return 1;
}


static struct dev_layer *_create_dev(struct dev_manager *dm, char *name,
				     char *dlid)
{
	struct dev_layer *dl;
	char *uuid;

	if (!(dl = pool_zalloc(dm->mem, sizeof(*dl)))) {
		stack;
		return NULL;
	}

	dl->name = name;

	log_debug("Getting device info for %s", dl->name);
	if (!_info(dl->name, dlid, &dl->info, dm->mem, &uuid)) {
		stack;
		return NULL;
	}

	if (dl->info.exists)
		dl->dlid = uuid;
	else
		dl->dlid = dlid;

	list_init(&dl->pre_create);

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

	if (!(name = _build_name(dm->mem, lv->vg->name, lv->name, layer))) {
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

static int _expand_vanilla(struct dev_manager *dm, struct logical_volume *lv)
{
	/*
	 * only one layer.
	 */
	struct dev_layer *dl;

	if (!(dl = _create_layer(dm, NULL, lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_vanilla;
	_set_flag(dl, VISIBLE);

	return 1;
}

static int _expand_origin_real(struct dev_manager *dm,
			       struct logical_volume *lv)
{
	struct dev_layer *dl;
	char *real_dlid;

	if (!(dl = _create_layer(dm, "real", lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_vanilla;
	_clear_flag(dl, VISIBLE);

	real_dlid = dl->dlid;

	if (!(dl = _create_layer(dm, NULL, lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_origin;
	_set_flag(dl, VISIBLE);

	/* add the dependency on the real device */
	if (!_pre_list_add(dm->mem, &dl->pre_create,
			   pool_strdup(dm->mem, real_dlid))) {
		stack;
		return 0;
	}

	return 1;
}

static int _expand_origin(struct dev_manager *dm, struct logical_volume *lv)
{
	struct logical_volume *active;
	struct snapshot *s;
	struct list *sh;

	/*
	 * We only need to create an origin layer if one of our
	 * snapshots is in the active list.
	 */
	list_iterate(sh, &dm->active_list) {
		active = list_item(sh, struct lv_list)->lv;
		if ((s = find_cow(active)) && (s->origin == lv))
			return _expand_origin_real(dm, lv);
	}

	return _expand_vanilla(dm, lv);
}

static int _expand_snapshot(struct dev_manager *dm, struct logical_volume *lv,
			    struct snapshot *s)
{
	/*
	 * snapshot(org, cow)
	 * cow
	 */
	struct dev_layer *dl;
	char *cow_dlid;

	if (!(dl = _create_layer(dm, "cow", lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_vanilla;
	_clear_flag(dl, VISIBLE);

	cow_dlid = dl->dlid;

	if (!(dl = _create_layer(dm, NULL, lv))) {
		stack;
		return 0;
	}
	dl->populate = _populate_snapshot;
	_set_flag(dl, VISIBLE);

	/* add the dependency on the real device */
	if (!_pre_list_add(dm->mem, &dl->pre_create,
			   pool_strdup(dm->mem, cow_dlid))) {
		stack;
		return 0;
	}

	/* add the dependency on the org device */
	if (!_pre_list_add(dm->mem, &dl->pre_create,
			   _build_dlid(dm->mem, s->origin->lvid.s, "real"))) {
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
	struct snapshot *s;

	/*
	 * FIXME: this doesn't cope with recursive snapshots yet.
	 */
	if ((s = find_cow(lv)))
		return _expand_snapshot(dm, lv, s);

	else if (lv_is_origin(lv))
		return _expand_origin(dm, lv);

	return _expand_vanilla(dm, lv);
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
	char *dlid;
	struct dev_layer *dep;

	list_iterate(sh, &dl->pre_create) {
		dlid = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, dlid))) {
			log_error("Couldn't find device layer '%s'.", dlid);
			return 0;
		}

		if (_get_flag(dep, flag))
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

static inline int _suspend_parent(struct dev_layer *parent)
{
	return (!parent || !parent->info.exists || _suspend(parent));
}

/*
 * Recurses through the tree, ensuring that devices are created
 * in correct order.
 */
int _create_rec(struct dev_manager *dm, struct dev_layer *dl,
		struct dev_layer *parent)
{
	struct list *sh;
	struct dev_layer *dep;
	char *dlid, *newname, *suffix;

	/* FIXME Create and use a _suspend_parents() function instead */
	/* Suspend? */
	if (_get_flag(dl, SUSPENDED) && 
	    (!_suspend_parent || !_suspend(dl))) {
		stack;
		return 0;
	}

	list_iterate(sh, &dl->pre_create) {
		dlid = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, dlid))) {
			log_error("Couldn't find device layer '%s'.", dlid);
			return 0;
		}

		if (!_suspend_parent(parent)) {
			stack;
			return 0;
		}

		if (!strcmp(dep->dlid, dl->dlid)) {
			log_error("BUG: pre-create loop detected (%s)", dlid);
			return 0;
		}

		if (!_create_rec(dm, dep, dl)) {
			stack;
			return 0;
		}
	}

	/* Rename? */
	if (dl->info.exists) {
		if ((suffix = rindex(dl->dlid, '-')))
			suffix++;
		newname = _build_name(dm->mem, dm->vg_name, dl->lv->name,
				      suffix);
		if (strcmp(newname, dl->name)) {
			if (!_suspend_parent(parent) || 
			    !_suspend(dl) ||
			    !_rename(dm, dl, newname)) {
				stack;
				return 0;
			}
		}
	}

	/* Create? */
	if (!dl->info.exists) {
		if (!_suspend_parent(parent) || 
		    !_load(dm, dl, DM_DEVICE_CREATE)) {
			stack;
			return 0;
		}
		return 1;
	}

	/* Reload? */
	if (_get_flag(dl, RELOAD) &&
	    (!_suspend_parent(parent) || !_suspend(dl) || 
	     !_load(dm, dl, DM_DEVICE_RELOAD))) {
		stack;
		return 0;
	}

	/* Resume? */
	if (!_get_flag(dl, SUSPENDED) && 
	    (!_suspend_parent || !_resume(dl))) {
		stack;
		return 0;
	}

	return 1;
}

static int _build_all_layers(struct dev_manager *dm, struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lvt;

	/*
	 * Build layers for complete vg.
	 */
	list_iterate(lvh, &vg->lvs) {
		lvt = list_item(lvh, struct lv_list)->lv;
		if (!_expand_lv(dm, lvt)) {
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

/*
 * Layers are removed in a top-down manner.
 */
int _remove_old_layers(struct dev_manager *dm)
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

	/*
	 * Now only top level devices will be unmarked.
	 */
	hash_iterate(hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);

		if (_get_flag(dl, ACTIVE) && _get_flag(dl, VISIBLE))
			_create_rec(dm, dl, NULL);
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
	const char *dev_dir = dm_dir();

	int r = 1;
	const char *name;
	struct dirent *dirent;
	DIR *d;

	if (!(d = opendir(dev_dir))) {
		log_sys_error("opendir", dev_dir);
		return 0;
	}

	while ((dirent = readdir(d))) {
		name = dirent->d_name;

		if (name[0] == '.')
			continue;

		/*
		 * Does this layer belong to us ?
		 */
		if (_belong_to_vg(dm->vg_name, name) &&
		    !_add_existing_layer(dm, name)) {
			stack;
			r = 0;
			break;
		}
	}

	if (closedir(d))
		log_sys_error("closedir", dev_dir);

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
	struct snapshot *s;
	struct list *lvh;

	list_iterate(lvh, &origin->vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		if ((s = find_cow(lv)) && s->origin == origin)
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

/* Remove any snapshots with given origin */
static void _remove_lvs(struct list *head, struct logical_volume *origin)
{
	struct logical_volume *active;
	struct snapshot *s;
	struct list *sh;

	list_iterate(sh, head) {
		active = list_item(sh, struct lv_list)->lv;
		if ((s = find_cow(active)) && s->origin == origin)
			_remove_lv(head, active);
	}

	_remove_lv(head, origin);
}

static int _fill_in_active_list(struct dev_manager *dm, struct volume_group *vg)
{
	int found;
	char *dlid;
	struct list *lvh;
	struct logical_volume *lv;

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		if (!(dlid = _build_dlid(dm->mem, lv->lvid.s, NULL))) {
			stack;
			return 0;
		}

		found = hash_lookup(dm->layers, dlid) ? 1 : 0;
		pool_free(dm->mem, dlid);

		if (found) {
			log_debug("Found active lv %s", lv->name);

			if (!_add_lv(dm->mem, &dm->active_list, lv)) {
				stack;
				return 0;
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
		_remove_lvs(&dm->active_list, lv);

	if (action == ACTIVATE) {
		/* Add to active & reload lists */
		if (!_add_lvs(dm->mem, &dm->reload_list, lv) ||
		    !_add_lvs(dm->mem, &dm->active_list, lv)) {
			stack;
			return 0;
		}
	}

	if (action == SUSPEND) {
		if (!_add_lvs(dm->mem, &dm->suspend_list, lv)) {
			stack;
			return 0;
		}
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

