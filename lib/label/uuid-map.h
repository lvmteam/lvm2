/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_UUID_MAP_H
#define _LVM_UUID_MAP_H

#include "uuid.h"
#include "dev-cache.h"
#include "pool.h"

/*
 * Holds a mapping from uuid -> device.
 */
struct uuid_map;

struct uuid_map *uuid_map_create(struct dev_filter *devices);
void uuid_map_destroy(struct uuid_map *um);

/*
 * Find the device with a particular uuid.
 */
struct device *uuid_map_lookup(struct uuid_map *um, struct id *id);
struct id *uuid_map_lookup_label(struct pool *mem, struct uuid_map *um, 
				 const char *name);

#endif
