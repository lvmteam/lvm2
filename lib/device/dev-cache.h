/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEV_CACHE_H
#define _LVM_DEV_CACHE_H

#include <sys/types.h>
#include "lvm-types.h"
#include "device.h"

/*
 * predicate for devices.
 */
struct dev_filter {
	int (*passes_filter)(struct dev_filter *f, struct device *dev);
	void (*destroy)(struct dev_filter *f);
	void *private;
};


/*
 * The global device cache.
 */
int dev_cache_init(void);
void dev_cache_exit(void);

/* Trigger(1) or avoid(0) a scan */
void dev_cache_scan(int do_scan);
int dev_cache_has_scanned(void);

int dev_cache_add_dir(const char *path);
struct device *dev_cache_get(const char *name, struct dev_filter *f);


/*
 * Object for iterating through the cache.
 */
struct dev_iter;
struct dev_iter *dev_iter_create(struct dev_filter *f);
void dev_iter_destroy(struct dev_iter *iter);
struct device *dev_iter_get(struct dev_iter *iter);

#endif
