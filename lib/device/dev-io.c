/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 */

#include "device.h"
#include "lvm-types.h"
#include "log.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

int dev_get_size(struct device *dev, uint64_t *size)
{
	int fd;
	long s;

	log_verbose("Getting device size");
	if ((fd = open(dev->name, O_RDONLY)) < 0) {
		log_sys_err("open");
		return 0;
	}

	/* FIXME: add 64 bit ioctl */
	if (ioctl(fd, BLKGETSIZE, &s) < 0) {
		log_sys_err("ioctl");
		close(fd);
		return 0;
	}

	close(fd);
	*size = (uint64_t) s;
	return 1;
}

int64_t dev_read(struct device *dev, uint64_t offset,
		 int64_t len, void *buffer)
{
	// FIXME: lazy programmer
	return 0;
}

int64_t dev_write(struct device *dev, uint64_t offset,
		  int64_t len, void *buffer)
{
	// FIXME: lazy programmer
	return 0;
}
