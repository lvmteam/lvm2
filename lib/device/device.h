/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEVICE_H
#define _LVM_DEVICE_H

#include "lvm-types.h"
#include "list.h"

#define DEV_ACCESSED_W		0x00000001	/* Device written to? */

/*
 * All devices in LVM will be represented by one of these.
 * pointer comparisons are valid.
 */
struct device {
	struct list aliases; /* struct str_list from lvm-types.h */
	dev_t dev;

	/* private */
	int fd;
	uint32_t flags;
};

struct device_list {
	struct list list;
	struct device *dev;
};

/*
 * All io should use these routines.
 */
int dev_get_size(struct device *dev, uint64_t *size);
int dev_get_sectsize(struct device *dev, uint32_t *size);

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

/* Return a valid device name from the alias list; NULL otherwise */
const char *dev_name_confirmed(struct device *dev);

static inline int is_lvm_partition(const char *name) {
	return 1;
}

#endif

