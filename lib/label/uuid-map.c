/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_UUID_MAP_H
#define _LVM_UUID_MAP_H

#include "uuid-map.h"
#include "dev-cache.h"
#include "dbg_malloc.h"
#include "log.h"
#include "label.h"

struct uuid_map {
	struct dev_filter *filter;
};


struct uuid_map *uuid_map_create(struct dev_filter *devices)
{
	struct uuid_map *um;

	if (!(um = dbg_malloc(sizeof(*um)))) {
		log_err("Couldn't allocate uuid_map object.");
		return NULL;
	}

	um->filter = devices;
	return um;
}

void uuid_map_destroy(struct uuid_map *um)
{
	dbg_free(um);
}

/*
 * Simple, non-caching implementation to start with.
 */
struct device *uuid_map_lookup(struct uuid_map *um, struct id *id)
{
	struct dev_iter *iter;
	struct device *dev;
	struct label *lab;

	if (!(iter = dev_iter_create(um->filter))) {
		stack;
		return NULL;
	}

	while ((dev = dev_iter_get(iter))) {

		if (!label_read(dev, &lab))
			continue;

		if (id_equal(id, &lab->id)) {
			label_destroy(lab);
			break;
		}

		label_destroy(lab);
	}

	dev_iter_destroy(iter);
	return dev;
}

#endif
