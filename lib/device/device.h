/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEVICE_H
#define _LVM_DEVICE_H

#include "uuid.h"

#define DEV_ACCESSED_W		0x00000001	/* Device written to? */

/*
 * All devices in LVM will be represented by one of these.
 * pointer comparisons are valid.
 */
struct device {
	struct list aliases;	/* struct str_list from lvm-types.h */
	dev_t dev;

	/* private */
	int fd;
	uint32_t flags;

	char pvid[ID_LEN + 1];
};

struct device_list {
	struct list list;
	struct device *dev;
};

struct device_area {
	struct device *dev;
	uint64_t start;		/* Bytes */
	uint64_t size;		/* Bytes */
};

/*
 * All io should use these routines.
 */
int dev_get_size(struct device *dev, uint64_t *size);
int dev_get_sectsize(struct device *dev, uint32_t *size);

int dev_open(struct device *dev, int flags);
int dev_close(struct device *dev);

static inline int dev_fd(struct device *dev)
{
	return dev->fd;
}

ssize_t raw_read(int fd, void *buf, size_t count);

ssize_t dev_read(struct device *dev, uint64_t offset, size_t len, void *buffer);
int64_t dev_write(struct device *dev, uint64_t offset, size_t len,
		  void *buffer);
int dev_zero(struct device *dev, uint64_t offset, size_t len);

static inline const char *dev_name(const struct device *dev)
{
	return (dev) ? list_item(dev->aliases.n, struct str_list)->str :
	    "unknown device";
}

/* Return a valid device name from the alias list; NULL otherwise */
const char *dev_name_confirmed(struct device *dev);

static inline int dev_is_open(struct device *dev)
{
	return dev->fd >= 0 ? 1 : 0;
}

/* FIXME Check partition type if appropriate */

#define is_lvm_partition(a) 1

/*
static inline int is_lvm_partition(const char *name)
{
	return 1;
}
*/

#endif
