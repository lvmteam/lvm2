/*
 * tools/lib/dev-manager.h
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEV_CACHE_H
#define _LVM_DEV_CACHE_H

/*
 * All devices in LVM will be represented by one of these.
 * pointer comparisons are valid.
 */
struct device {
	char *name;
	dev_t dev;
};

struct dev_filter {
	int (*fn)(struct device *dev, struct dev_cache_filter *f);
	void *private;
};


/*
 * The global device cache.
 */
int dev_cache_init(void);
void dev_cache_exit(void);

int dev_cache_add_dir(const char *path);
struct device *dev_cache_get(const char *name);


/*
 * Object for iterating through the cache.
 */
struct dev_iter;
struct dev_iter *dev_iter_create(struct dev_filter *f);
void dev_iter_destroy(struct dev_iter *iter);
struct device *dev_iter_get(struct dev_iter *iter);


/*
 * All io should use these routines, rather than opening the devices
 * by hand.  You do not have to call an open routine.
 */
uint64_t dev_get_size(struct device *dev); /* in 512 byte sectors */
ssize_t dev_read(struct device *dev, size_t offset, size_t len, void *buffer);
ssize_t dev_write(struct device *dev, size_t offset, size_t len, void *buffer);

#endif
