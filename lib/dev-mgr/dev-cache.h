/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEV_CACHE_H
#define _LVM_DEV_CACHE_H

#include <sys/types.h>

/*
 * All devices in LVM will be represented by one of these.
 * pointer comparisons are valid.
 */
struct device {
	char *name;
	dev_t dev;
};

struct dev_filter {
	int (*passes_filter)(struct dev_filter *f, struct device *dev);
	void *private;
};


/*
 * The global device cache.
 */
int dev_cache_init(void);
void dev_cache_exit(void);

int dev_cache_add_dir(const char *path);
struct device *dev_cache_get(const char *name, struct dev_filter *f);


/*
 * Object for iterating through the cache.
 */
struct dev_iter;
struct dev_iter *dev_iter_create(struct dev_filter *f);
void dev_iter_destroy(struct dev_iter *iter);
struct device *dev_iter_get(struct dev_iter *iter);


/*
 * All io should use these routines, rather than opening the devices
 * by hand.  You do not have to call an open routine.  ATM all io is
 * immediately flushed.
 */
int dev_get_size(struct device *dev, uint64_t *size); /* in k */
int64_t dev_read(struct device *dev, uint64_t offset,
		 int64_t len, void *buffer);
int64_t dev_write(struct device *dev, uint64_t offset,
		  int64_t len, void *buffer);

#endif
