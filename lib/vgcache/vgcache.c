/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include <stdlib.h>
#include "vgcache.h"
#include "hash.h"
#include "dbg_malloc.h"
#include "log.h"

static struct hash_table *_vghash;

/* NULL is a special case that returns all devices */
const char *null_const = "\0";

int vgcache_init()
{
	if (!(_vghash = hash_create(128))) {
		return 0;
	}

	return 1;
}

struct list *vgcache_find(const char *vg_name)
{
	struct vgname_entry *vgn;

	if (!_vghash)
		return NULL;

	if (!vg_name)
		vg_name = null_const;

	if (!(vgn = hash_lookup(_vghash, vg_name)))
		return NULL;

	return &vgn->pvdevs;
}

int vgcache_add_entry(const char *vg_name, struct device *dev)
{

	struct vgname_entry *vgn;
	struct pvdev_list *pvdev;
	struct list *pvdh, *pvdevs;

	if (!(pvdevs = vgcache_find(vg_name))) {
		if (!(vgn = dbg_malloc(sizeof(struct vgname_entry)))) {
			log_error("struct vgname_entry allocation failed");
			return 0;
		}

		pvdevs = &vgn->pvdevs;
		list_init(pvdevs);

		if (!(vgn->vgname = dbg_strdup(vg_name))) {
			log_error("vgcache_add: strdup vg_name failed");
			return 0;
		}

		if (!hash_insert(_vghash, vg_name, vgn)) {
			log_error("vgcache_add: hash insertion failed");
			return 0;
		}
	}

	list_iterate(pvdh, pvdevs) {
		pvdev = list_item(pvdh, struct pvdev_list);
		if (dev == pvdev->dev)
			return 1;
	}

	if (!(pvdev = dbg_malloc(sizeof(struct pvdev_list)))) {
		log_error("struct pvdev_list allocation failed");
		return 0;
	}

	pvdev->dev = dev;
	list_add(pvdevs, &pvdev->list);

	return 1;
}

int vgcache_add(const char *vg_name, struct device *dev)
{
	if (!_vghash && !vgcache_init())
		return 0;

	if (vg_name && !vgcache_add_entry(vg_name, dev))
		return 0;

	return vgcache_add_entry(null_const, dev);
}

void vgcache_destroy_entry(struct vgname_entry *vgn)
{
	struct list *pvdh;
	struct pvdev_list *pvdev;

	if (vgn) {
		pvdh = vgn->pvdevs.n;
		while (pvdh != &vgn->pvdevs) {
			pvdev = list_item(pvdh, struct pvdev_list);
			pvdh = pvdh->n;
			dbg_free(pvdev);
		}
		dbg_free(vgn->vgname);
	}
	dbg_free(vgn);
}

void vgcache_del(const char *vg_name)
{
	struct vgname_entry *vgn;

	if (!_vghash)
		return;

	if (!vg_name)
		vg_name = null_const;

	if (!(vgn = hash_lookup(_vghash, vg_name)))
		return;

	hash_remove(_vghash, vg_name);
	vgcache_destroy_entry(vgn);
}

void vgcache_destroy()
{
	if (_vghash) {
		hash_iterate(_vghash, (iterate_fn)vgcache_destroy_entry);
		hash_destroy(_vghash);
	}
}
