/*
 * tools/lib/dev-manager.h
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 */

#ifndef DEV_CACHE_H
#define DEV_CACHE_H

struct device {
	char *name;
	dev_t dev;
};

int init_dev_cache();
void exit_dev_cache();

struct device *get_device(const char *name);
void put_device(struct device *d);

/*
 * All io should use these routines, rather than opening the devices
 * by hand.  You do not have to call an open routine.
 */
ssize_t dev_read(struct device *dev, size_t offset, size_t len, void *buffer);
ssize_t dev_write(struct device *dev, size_t offset, size_t len, void *buffer);

#endif
