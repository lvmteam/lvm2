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

/* layer types */
enum {
	VANILLA,
	ORIGIN,
	SNAPSHOT
};

struct dev_layer {
	char *name;
	int mark;
	int visible;

	/*
	 * Setup the dm_task.
	 */
	int type;
	int (*populate)(struct dev_manager *dm,
			struct dm_task *dmt, struct dev_layer *dl);
	struct dm_info info;
	struct logical_volume *lv;

	/*
	 * Devices that must be created before this one can be
	 * created.  Holds str_lists.
	 */
	 struct list pre_create;

	/*
	 * Devices that must be created before this one can be
	 * unsuspended.  Holds str_lists.
	 */
	 struct list pre_active;
};

struct dev_manager {
	struct pool *mem;

	char *vg_name;
	struct hash_table *layers;
};


/*
 * Device layer names are all of the form <vg>-<lv>-<layer>, any
 * other hyphens that appear in these names are quoted with yet
 * another hyphen.  The top layer of any device is always called
 * 'top'.  eg, vg0-lvol0-top.
 */
static void _count_hyphens(const char *str, size_t *len, int *hyphens)
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

static char *_build_name(struct pool *mem, const char *vg,
			 const char *lv, const char *layer)
{
	size_t len = 0;
	int hyphens = 0;
	char *r, *out;

	_count_hyphens(vg, &len, &hyphens);
	_count_hyphens(lv, &len, &hyphens);
	_count_hyphens(layer, &len, &hyphens);

	len += hyphens + 2;

	if (!(r = pool_alloc(mem, len))) {
		stack;
		return NULL;
	}

	out = r;
	_quote_hyphens(&out, vg); *out++ = '-';
	_quote_hyphens(&out, lv); *out++ = '-';
	_quote_hyphens(&out, layer); *out = '\0';

	return r;
}


/*
 * Low level device-layer operations.
 */
static struct dm_task *_setup_task(const char *name, int task)
{
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task))) {
		stack;
		return NULL;
	}

	dm_task_set_name(dmt, name);
	return dmt;
}


static int _load(struct dev_manager *dm, struct dev_layer *dl, int task)
{
	int r;
	struct dm_task *dmt;

	log_very_verbose("Loading %s", dl->name);
	if (!(dmt = _setup_task(dl->name, task))) {
		stack;
		return 0;
	}

	/*
	 * Populate the table.
	 */
	if (!dl->populate(dm, dmt, dl)) {
		log_err("Couldn't populate device '%s'.", dl->name);
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_err("Couldn't load device '%s'.", dl->name);
	dm_task_destroy(dmt);

	if (dl->visible)
		fs_add_lv(dl->lv, dl->name);

	return r;
}

static int _remove(struct dev_layer *dl)
{
	int r;
	struct dm_task *dmt;

	log_very_verbose("Removing %s", dl->name);
	if (!(dmt = _setup_task(dl->name, DM_DEVICE_REMOVE))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_err("Couldn't remove device '%s'", dl->name);

	dm_task_destroy(dmt);

	if (dl->visible)
		fs_del_lv(dl->lv);

	return r;
}

static int _suspend_or_resume(const char *name, int sus)
{
	int r;
	struct dm_task *dmt;
	int task = sus ? DM_DEVICE_SUSPEND : DM_DEVICE_RESUME;

	log_very_verbose("%s %s", sus ? "Suspending" : "Resuming", name);
	if (!(dmt = _setup_task(name, task))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_err("Couldn't %s device '%s'", sus ? "suspend" : "resume",
			name);

	dm_task_destroy(dmt);
	return r;
}

static int _suspend(struct dev_layer *dl)
{
	if (dl->info.suspended)
		return 1;

	if (!_suspend_or_resume(dl->name, 1)) {
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

	if (!_suspend_or_resume(dl->name, 0)) {
		stack;
		return 0;
	}

	dl->info.suspended = 0;
	return 1;
}

static int _info(const char *name, struct dm_info *info)
{
	int r = 0;
	struct dm_task *dmt;

	log_very_verbose("Getting device info for %s", name);
	if (!(dmt = _setup_task(name, DM_DEVICE_INFO))) {
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
	r = 1;

 out:
	dm_task_destroy(dmt);
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

	if (!(real = _build_name(dm->mem, dm->vg_name,
				 dl->lv->name, "real"))) {
		stack;
		return 0;
	}

	if (lvm_snprintf(params, sizeof(params), "%s/%s 0",
			 dm_dir(), real) == -1) {
		log_err("Couldn't create origin device parameters for '%s'.",
			dl->name);
		return 0;
	}

	log_very_verbose("Adding target: 0 %" PRIu64 " snapshot-origin %s",
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
		log_err("Couldn't find snapshot for '%s'.", dl->name);
		return 0;
	}

	if (!(origin = _build_name(dm->mem, dm->vg_name,
				   s->origin->name, "real"))) {
		stack;
		return 0;
	}

	if (!(cow = _build_name(dm->mem, dm->vg_name,
				s->cow->name, "cow"))) {
		stack;
		return 0;
	}

        if (snprintf(params, sizeof(params), "%s/%s %s/%s P %d 128",
		     dm_dir(), origin, dm_dir(), cow, s->chunk_size) == -1) {
                stack;
                return 0;
        }

	log_very_verbose("Adding target: 0 %" PRIu64 " snapshot %s",
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
	if (!(name = _build_name(dm->mem, lv->vg->name, lv->name, "top"))) {
		stack;
		return 0;
	}

	/*
	 * Try and get some info on this device.
	 */
	if (!_info(name, info)) {
		stack;
		return 0;
	}

	return 1;
}

static struct dev_layer *
_create_layer(struct pool *mem, const char *layer,
	      int type, struct logical_volume *lv)
{
	struct dev_layer *dl;

	if (!(dl = pool_zalloc(mem, sizeof(*dl)))) {
		stack;
		return NULL;
	}

	if (!(dl->name = _build_name(mem, lv->vg->name, lv->name, layer))) {
		stack;
		return NULL;
	}

	if (!_info(dl->name, &dl->info)) {
		stack;
		return NULL;
	}

	dl->type = type;
	dl->lv = lv;
	list_init(&dl->pre_create);
	list_init(&dl->pre_active);

	return dl;
}

/*
 * Finds the specified layer.
 */
static struct dev_layer *_lookup(struct dev_manager *dm,
				 const char *lv, const char *layer)
{
	char *name;
	struct dev_layer *dl;

	if (!(name = _build_name(dm->mem, dm->vg_name, lv, layer))) {
		stack;
		return NULL;
	}

	dl = hash_lookup(dm->layers, name);
	pool_free(dm->mem, name);
	return dl;
}


/*
 * Inserts the appropriate dev_layers for a logical volume.
 */
static int _expand_lv(struct dev_manager *dm, struct logical_volume *lv)
{
	struct snapshot *s;

	/*
	 * FIXME: this doesn't cope with recursive snapshots yet.
	 * FIXME: split this function up.
	 */
	if ((s = find_cow(lv))) {
		/*
		 * snapshot(org, cow)
		 * cow
		 */
		struct dev_layer *dl;
		char *cow_name;
		struct str_list *sl;

		if (!(dl = _create_layer(dm->mem, "cow", VANILLA, lv))) {
			stack;
			return 0;
		}
		dl->populate = _populate_vanilla;
		dl->visible = 0;

		/* insert the cow layer */
		if (!hash_insert(dm->layers, dl->name, dl)) {
			stack;
			return 0;
		}
		cow_name = dl->name;

		if (!(dl = _create_layer(dm->mem, "top", SNAPSHOT, lv))) {
			stack;
			return 0;
		}
		dl->populate = _populate_snapshot;
		dl->visible = 1;

		/* add the dependency on the real device */
		if (!(sl = pool_alloc(dm->mem, sizeof(*sl)))) {
			stack;
			return 0;
		}

		if (!(sl->str = pool_strdup(dm->mem, cow_name))) {
			stack;
			return 0;
		}

		list_add(&dl->pre_create, &sl->list);

		/* add the dependency on the org device */
		if (!(sl = pool_alloc(dm->mem, sizeof(*sl)))) {
			stack;
			return 0;
		}

		if (!(sl->str = _build_name(dm->mem, dm->vg_name,
					    s->origin->name, "top"))) {
			stack;
			return 0;
		}

		list_add(&dl->pre_create, &sl->list);

		/* insert the snapshot layer */
		if (!hash_insert(dm->layers,dl->name, dl)) {
			stack;
			return 0;
		}

	} else if (lv_is_origin(lv)) {
		/*
		 * origin(org)
		 * org
		 */
		struct dev_layer *dl;
		char *real_name;
		struct str_list *sl;

		if (!(dl = _create_layer(dm->mem, "real", VANILLA, lv))) {
			stack;
			return 0;
		}
		dl->populate = _populate_vanilla;
		dl->visible = 0;

		if (!hash_insert(dm->layers, dl->name, dl)) {
			stack;
			return 0;
		}
		real_name = dl->name;

		if (!(dl = _create_layer(dm->mem, "top", ORIGIN, lv))) {
			stack;
			return 0;
		}
		dl->populate = _populate_origin;
		dl->visible = 1;

		/* add the dependency on the real device */
		if (!(sl = pool_alloc(dm->mem, sizeof(*sl)))) {
			stack;
			return 0;
		}

		if (!(sl->str = pool_strdup(dm->mem, real_name))) {
			stack;
			return 0;
		}

		list_add(&dl->pre_create, &sl->list);

		if (!hash_insert(dm->layers,dl->name, dl)) {
			stack;
			return 0;
		}

	} else {
		/*
		 * only one layer.
		 */
		struct dev_layer *dl;
		if (!(dl = _create_layer(dm->mem, "top", VANILLA, lv))) {
			stack;
			return 0;
		}
		dl->populate = _populate_vanilla;
		dl->visible = 1;

		if (!hash_insert(dm->layers, dl->name, dl)) {
			stack;
			return 0;
		}
	}

	return 1;
}

/*
 * Clears the mark bit on all layers.
 */
static void _clear_marks(struct dev_manager *dm)
{
	struct hash_node *hn;
	struct dev_layer *dl;

	hash_iterate (hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);
		dl->mark = 0;
	}
}


/*
 * Starting with a given layer this function recurses through all
 * dependent layers setting the mark bit.
 */
static int _mark_pre_create(struct dev_manager *dm, struct dev_layer *dl)
{
	struct list *sh;
	char *name;
	struct dev_layer *dep;

	list_iterate (sh, &dl->pre_create) {
		name = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, name))) {
			log_err("Couldn't find device layer '%s'.", name);
			return 0;
		}

		if (dep->mark)
			continue;

		dep->mark = 1;

		if (!_mark_pre_create(dm, dep)) {
			stack;
			return 0;
		}
	}

	return 1;
}

void _emit(struct dev_layer *dl)
{
	log_print("emitting layer '%s'", dl->name);
}

/*
 * Recurses through the tree, ensuring that devices are created
 * in correct order.
 */
int _create_rec(struct dev_manager *dm, struct dev_layer *dl)
{
	struct list *sh;
	struct dev_layer *dep;
	char *name;

	if (dl->info.exists && !_suspend(dl)) {
		stack;
		return 0;
	}

	list_iterate (sh, &dl->pre_create) {
		name = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, name))) {
			log_err("Couldn't find device layer '%s'.", name);
			return 0;
		}

		if (!_create_rec(dm, dep)) {
			stack;
			return 0;
		}
	}

	if (dl->info.exists) {
		/* reload */
		if (!_load(dm, dl, DM_DEVICE_RELOAD)) {
			stack;
			return 0;
		}

		if (!_resume(dl)) {
			stack;
			return 0;
		}
	} else {
		/* create */
		if (!_load(dm, dl, DM_DEVICE_CREATE)) {
			stack;
			return 0;
		}
	}

	return 1;
}

/*
 * Layers are removed in a top-down manner.
 */
int _remove_rec(struct dev_manager *dm, struct dev_layer *dl)
{
	struct list *sh;
	struct dev_layer *dep;
	char *name;

	if (dl->info.exists && dl->info.suspended && !_resume(dl)) {
		stack;
		return 0;
	}

	if (!_remove(dl)) {
		stack;
		return 0;
	}

	list_iterate (sh, &dl->pre_create) {
		name = list_item(sh, struct str_list)->str;

		if (!(dep = hash_lookup(dm->layers, name))) {
			log_err("Couldn't find device layer '%s'.", name);
			return 0;
		}

		if (!_remove_rec(dm, dep)) {
			stack;
			return 0;
		}
	}

	return 1;
}

static int _mark_dependants(struct dev_manager *dm)
{
	struct hash_node *hn;
	struct dev_layer *dl;

	_clear_marks(dm);

	/*
	 * Mark any dependants.
	 */
	hash_iterate (hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);

		if (!dl->mark) {
			if (!_mark_pre_create(dm, dl)) {
				stack;
				return 0;
			}

			if (dl->mark) {
				log_err("Circular device dependency found for "
					"'%s'.",
					dl->name);
				return 0;
			}
		}
	}

	return 1;
}

/*
 * Remove all layers from the hash table that do not have their
 * mark flag set.
 */
static int _prune_unmarked(struct dev_manager *dm)
{
	struct hash_node *hn, *next;
	struct dev_layer *dl;

	for (hn = hash_get_first(dm->layers); hn; hn = next) {

		next = hash_get_next(dm->layers, hn);
		dl = hash_get_data(dm->layers, hn);

		if (!dl->mark)
			hash_remove(dm->layers, dl->name);
	}

	return 1;
}

static int _select_lv(struct dev_manager *dm, struct logical_volume *lv)
{
	struct dev_layer *dl;
	struct list *lvh;

	/*
	 * Build layers for complete vg.
	 */
	list_iterate (lvh, &lv->vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;
		if (!_expand_lv(dm, lv)) {
			stack;
			return 0;
		}
	}

	/*
	 * Mark the desired logical volume.
	 */
	if (!(dl = _lookup(dm, lv->name, "top"))) {
		log_err("Couldn't find top layer of '%s'.", lv->name);
		return 0;
	}

	dl->mark = 1;
	if (!_mark_pre_create(dm, dl)) {
		stack;
		return 0;
	}

	_prune_unmarked(dm);
	return 1;
}

/*
 * The guts of the activation unit, this examines the device
 * layers in the manager, and tries to issue the correct
 * instructions to activate them in order.
 */
static int _execute(struct dev_manager *dm, struct logical_volume *lv,
		    int (*cmd)(struct dev_manager *dm, struct dev_layer *dl))
{
	struct hash_node *hn;
	struct dev_layer *dl;

	if (!_select_lv(dm, lv)) {
		stack;
		return 0;
	}

	/*
	 * We need to make a list of top level devices, ie. those
	 * that have no entries in 'pre_create'.
	 */
	if (!_mark_dependants(dm)) {
		stack;
		return 0;
	}

	/*
	 * Now only top level devices will be unmarked.
	 */
	hash_iterate (hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);

		if (!dl->mark)
			cmd(dm, dl);
	}

	return 1;
}



int dev_manager_activate(struct dev_manager *dm, struct logical_volume *lv)
{
	if (!_execute(dm, lv, _create_rec)) {
		stack;
		return 0;
	}

	return 1;
}

int dev_manager_reactivate(struct dev_manager *dm, struct logical_volume *lv)
{
	if (!_execute(dm, lv, _create_rec)) {
		stack;
		return 0;
	}

	return 1;
}

int dev_manager_deactivate(struct dev_manager *dm, struct logical_volume *lv)
{
	if (!_execute(dm, lv, _remove_rec)) {
		stack;
		return 0;
	}

	return 0;
}
