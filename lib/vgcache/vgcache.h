/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#ifndef _LVM_VGCACHE_H
#define _LVM_VGCACHE_H

#include "vgcache.h"
#include "list.h"
#include "uuid.h"
#include "pool.h"
#include "dev-cache.h"
#include "metadata.h"


/*
 * Maintains a register of LVM specific information about
 * devices.  Makes use of the label code.
 */
struct vg_cache;

struct vg_cache *vg_cache_create(struct dev_filter *devices);
void vg_cache_destroy(struct vg_cache *vgc);

/*
 * Find the device with a particular uuid.
 */
struct device *vg_cache_find_uuid(struct vg_cache *vgc, struct id *id);

/*
 * Find all devices in a particular volume group.
 */
struct list *vg_cache_find_vg(struct vg_cache *vgc, struct pool *mem,
			      const char *vg);

/*
 * Tell the cache about any changes occuring on disk.
 * FIXME: it would be nice to do without these.
 */
int vg_cache_update_vg(struct volume_group *vg);
int vg_cache_update_device(struct device *dev);

#endif
