/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEVICE_H
#define _LVM_DEVICE_H

#include "lvm-types.h"
#include "list.h"

/*
 * All devices in LVM will be represented by one of these.
 * pointer comparisons are valid.
 */
struct device {
	struct list aliases; /* struct str_list from lvm-types.h */
	dev_t dev;

	/* private */
	int fd;
};

/*
 * All io should use these routines.
 */
int dev_get_size(struct device *dev, uint64_t *size);

int dev_open(struct device *dev, int flags);
int dev_close(struct device *dev);

int64_t dev_read(struct device *dev,
		 uint64_t offset, int64_t len, void *buffer);
int64_t dev_write(struct device *dev,
		  uint64_t offset, int64_t len, void *buffer);
int dev_zero(struct device *dev, uint64_t offset, int64_t len);


static inline const char *dev_name(struct device *dev) {
	return list_item(dev->aliases.n, struct str_list)->str;
}


static inline int is_lvm_partition(const char *name) {
	return 1;
}

#define LVM_DEFAULT_DIR_PREFIX "/dev/"
/* FIXME Allow config file override */
static inline char *lvm_dir_prefix(void) { return LVM_DEFAULT_DIR_PREFIX; }

#endif

