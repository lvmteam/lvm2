/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "dev_manager.h"
#include "pool.h"
#include "hash.h"
#include "log.h"

#include <libdevmapper.h>

/* layer types */
enum {
	VANILLA,
	ORIGIN,
	SNAPSHOT
};

struct dev_layer {
	char *name;
	int mark;

	/*
	 * Setup the dm_task.
	 */
	int type;
	int (*populate)(struct dm_task *dmt, struct dev_layer *dl);
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
 * Device layer names are all of the form <vg>:<lv>:<layer>, any
 * other colons that appear in these names are quoted with yet
 * another colon.  The top layer of any device is always called
 * 'top'.  eg, vg0:lvol0:top.
 */
static void _count_colons(const char *str, size_t *len, int *colons)
{
	const char *ptr;

	for (ptr = str; *ptr; ptr++, (*len)++)
		if (*ptr == ':')
			(*colons)++;
}

/*
 * Copies a string, quoting colons with colons.
 */
static void _quote_colons(char **out, const char *src)
{
	while (*src) {
		if (*src == ':')
			*(*out)++ = ':';

		*(*out)++ = *src++;
	}
}

static char *_build_name(struct pool *mem, const char *vg,
			 const char *lv, const char *layer)
{
	size_t len = 0;
	int colons = 0;
	char *r, *out;

	_count_colons(vg, &len, &colons);
	_count_colons(lv, &len, &colons);
	_count_colons(layer, &len, &colons);

	len += colons + 1;

	if (!(r = pool_alloc(mem, len))) {
		stack;
		return NULL;
	}

	out = r;
	_quote_colons(&out, vg);
	_quote_colons(&out, lv);
	_quote_colons(&out, layer);
	*out = '\0';

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


static int _load(struct dev_layer *dl, int task)
{
	int r;
	struct dm_task *dmt;

	log_very_verbose("Creating %s.", dl->name);
	if (!(dmt = _setup_task(dl->name, task))) {
		stack;
		return 0;
	}

	/*
	 * Populate the table.
	 */
	if (!dl->populate(dmt, dl)) {
		log_err("Couldn't populate device '%s'.", dl->name);
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_err("Couldn't create device '%s'.", dl->name);
	dm_task_destroy(dmt);

	return r;
}

static int _remove(struct dev_layer *dl)
{
	int r;
	struct dm_task *dmt;

	log_very_verbose("Removing device '%s'.", dl->name);
	if (!(dmt = _setup_task(dl->name, DM_DEVICE_REMOVE))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_err("Couldn't remove device '%s'", dl->name);

	dm_task_destroy(dmt);
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

	return _suspend_or_resume(dl->name, 1);
}

static int _resume(struct dev_layer *dl)
{
	if (!dl->info.suspended)
		return 1;

	return _suspend_or_resume(dl->name, 0);
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
 * Inserts the dev_layers for a logical volume.
 */
static int _expand_lv(struct dev_manager *dm, struct logical_volume *lv)
{
	struct snapshot *s;

	/*
	 * FIXME: this doesn't cope with recursive snapshots yet.
	 */
	if ((s = find_cow(lv))) {
		/*
		 * snapshot(org, cow)
		 * cow
		 */
		log_err("Snapshot devices not supported yet.");
		return 0;


	} else if (lv_is_origin(lv)) {
		/*
		 * origin(org)
		 * org
		 */
		log_err("Origin devices not supported yet.");
		return 0;

	} else {
		/*
		 * only one layer.
		 */
		struct dev_layer *dl;
		if (!(dl = _create_layer(dm->mem, "top", VANILLA, lv))) {
			stack;
			return 0;
		}

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
int _create(struct dev_manager *dm, struct dev_layer *dl)
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

		if (!_create(dm, dep)) {
			stack;
			return 0;
		}
	}

	if (dl->info.exists) {
		/* reload */
		if (!_load(dl, DM_DEVICE_RELOAD)) {
			stack;
			return 0;
		}

		if (!_resume(dl)) {
			stack;
			return 0;
		}
	} else {
		/* create */
		if (!_load(dl, DM_DEVICE_CREATE)) {
			stack;
			return 0;
		}
	}

	return 1;
}

/*
 * The guts of the activation unit, this examines the device
 * layers in the manager, and tries to issue the correct
 * instructions to activate them in order.
 */
static int _execute(struct dev_manager *dm)
{
	struct hash_node *hn;
	struct dev_layer *dl;

	/*
	 * We need to make a list of top level devices, ie. those
	 * that have no entries in 'pre_create'.
	 */
	_clear_marks(dm);

	/*
	 * Mark any dependents.
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

	/*
	 * Now only top level devices will be unmarked.
	 */
	hash_iterate (hn, dm->layers) {
		dl = hash_get_data(dm->layers, hn);

		if (!dl->mark)
			_emit(dl);
	}

	return 1;
}


/*
 * Remove all layers from the hash table that do not have their
 * mark flag set.
 */
static int _prune_unmarked(struct dev_manager *dm)
{
	struct hash_node *hn;
	struct dev_layer *dl;

	for (hn = hash_get_first(dm->layers); hn;
	     hn = hash_get_next(dm->layers, hn)) {
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
	dl = _lookup(dm, lv->name, "top");
	dl->mark = 1;
	if (!_mark_pre_create(dm, dl)) {
		stack;
		return 0;
	}

	_prune_unmarked(dm);
	return 1;
}
int dev_manager_activate(struct dev_manager *dm, struct logical_volume *lv)
{
	if (!_select_lv(dm, lv)) {
		stack;
		return 0;
	}

	if (!_execute(dm)) {
		stack;
		return 0;
	}

	return 1;
}

int dev_manager_reactivate(struct dev_manager *dm, struct logical_volume *lv)
{
	log_err("dev_manager_reactivate not implemented.");
	return 0;
}

int dev_manager_deactivate(struct dev_manager *dm, struct logical_volume *lv)
{
	if (!_select_lv(dm, lv)) {
		stack;
		return 0;
	}

	if (!_execute(dm)) {
		stack;
		return 0;
	}

	return 0;
}

