/*
 * Copyright (C) 2001, 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include <stdlib.h>

#include "vgcache.h"
#include "label.h"
#include "dbg_malloc.h"
#include "log.h"
#include "lvm1_label.h"


/*
 * Non-caching implementation.
 * FIXME: write caching version, when thought about it a bit more.
 */
struct vg_cache {
	struct dev_filter *filter;
};

struct vg_cache *vg_cache_create(struct dev_filter *devices)
{
	struct vg_cache *vgc;

	if (!(vgc = dbg_malloc(sizeof(*vgc)))) {
		log_err("Couldn't allocate vg_cache object.");
		return NULL;
	}

	vgc->filter = devices;

	return vgc;
}

void vg_cache_destroy(struct vg_cache *vgc)
{
	dbg_free(vgc);
}

struct device *vg_cache_find_uuid(struct vg_cache *vgc, struct id *id)
{
	struct dev_iter *iter;
	struct device *dev;
	struct label *lab;

	if (!(iter = dev_iter_create(vgc->filter))) {
		stack;
		return NULL;
	}

	while ((dev = dev_iter_get(iter))) {

		if (label_read(dev, &lab))
			continue;

		if (!strcmp(lab->volume_type, "lvm") && id_equal(id, &lab->id))
			break;

		label_destroy(lab);
	}

	dev_iter_destroy(iter);
	return dev;
}

static void _find_pvs_in_vg(struct vg_cache *vgc, struct pool *mem,
			    const char *vg, struct dev_iter *iter,
			    struct list *results)
{
	struct device *dev;
	struct label *lab;
	struct device_list *dev_list;
	struct lvm_label_info *info;

	while ((dev = dev_iter_get(iter))) {

		if (!label_read(dev, &lab))
			continue;

		if (strcmp(lab->volume_type, "lvm"))
			continue;

		info = (struct lvm_label_info *) lab->extra_info;

		if (!vg || strcmp(info->volume_group, vg)) {

			/* add dev to the result list */
			if (!(dev_list = pool_alloc(mem, sizeof(*dev_list)))) {
				stack;
				label_destroy(lab);
				return;
			}

			dev_list->dev = dev;
			list_add(results, &dev_list->list);
		}

		label_destroy(lab);
	}
}

struct list *vg_cache_find_vg(struct vg_cache *vgc, struct pool *mem,
			      const char *vg)
{
	struct dev_iter *iter;
	struct list *r;

	if (!(r = pool_alloc(mem, sizeof(r)))) {
		stack;
		return NULL;
	}
	list_init(r);

	if (!(iter = dev_iter_create(vgc->filter))) {
		stack;
		pool_free(mem, r);
		return NULL;
	}

	_find_pvs_in_vg(vgc, mem, vg, iter, r);

	dev_iter_destroy(iter);
	return r;
}

int vg_cache_update_vg(struct volume_group *vg)
{
	/* no-ops in a non caching version */
	return 1;
}

int vg_cache_update_device(struct device *dev)
{
	/* no-ops in a non caching version */
	return 1;
}
