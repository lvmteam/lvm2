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
	char *name;
	struct list_head aliases; /* struct str_list from lvm-types.h */
	dev_t dev;
};

/*
 * All io should use these routines, rather than opening the devices
 * by hand.  You do not have to call an open routine.  ATM all io is
 * immediately flushed.
 */
int dev_get_size(struct device *dev, uint64_t *size);
int64_t dev_read(struct device *dev,
		 uint64_t offset, int64_t len, void *buffer);
int64_t dev_write(struct device *dev,
		  uint64_t offset, int64_t len, void *buffer);


static inline int is_lvm_partition(const char *name) {
	return 1;
}

#define LVM_DEFAULT_DIR_PREFIX "/dev/"
/* FIXME Allow config file override */
static inline char *lvm_dir_prefix(void) { return LVM_DEFAULT_DIR_PREFIX; }

#endif

