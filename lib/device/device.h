/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEVICE_H
#define _LVM_DEVICE_H

#include "lvm-types.h"

/*
 * All devices in LVM will be represented by one of these.
 * pointer comparisons are valid.
 */
struct device {
	char *name;
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

/* FIXME: Alasdair lets add more query functions here for accessing
   the partition information, this can then be used by your filter. */

#endif

