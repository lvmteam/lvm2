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

	log_very_verbose("Getting size of %s", dev->name);
	if ((fd = open(dev->name, O_RDONLY)) < 0) {
		log_sys_error("open", dev->name);
		return 0;
	}

	/* FIXME: add 64 bit ioctl */
	if (ioctl(fd, BLKGETSIZE, &s) < 0) {
		log_sys_error("ioctl BLKGETSIZE", dev->name);
		close(fd);
		return 0;
	}

	close(fd);
	*size = (uint64_t) s;
	return 1;
}

/*
 *  FIXME: factor common code out.
 */

int _read(int fd, void *buf, size_t count)
{
	size_t n = 0;
	int tot = 0;

	while (tot < count) {
		do
			n = read(fd, buf, count - tot);
		while ((n < 0) && ((errno == EINTR) || (errno == EAGAIN)));

		if (n <= 0)
			return tot ? tot : n;

		tot += n;
		buf += n;
	}

	return tot;
}

int64_t dev_read(struct device *dev, uint64_t offset,
		 int64_t len, void *buffer)
{
	int64_t r;
	int fd = open(dev->name, O_RDONLY);

	if (fd < 0) {
		log_sys_very_verbose("open", dev->name);
		return 0;
	}

	if (lseek(fd, offset, SEEK_SET) < 0) {
		log_sys_error("lseek", dev->name);
		return 0;
	}

	r = _read(fd, buffer, len);
	close(fd);
	return r;
}

int _write(int fd, const void *buf, size_t count)
{
	size_t n = 0;
	int tot = 0;

	while (tot < count) {
		do
			n = write(fd, buf, count - tot);
		while ((n < 0) && ((errno == EINTR) || (errno == EAGAIN)));

		if (n <= 0)
			return tot ? tot : n;

		tot += n;
		buf += n;
	}

	return tot;
}

int64_t dev_write(struct device *dev, uint64_t offset,
		  int64_t len, void *buffer)
{
	int64_t r;
	int fd = open(dev->name, O_WRONLY);

	if (fd < 0) {
		log_sys_error("open", dev->name);
		return 0;
	}

	if (lseek(fd, offset, SEEK_SET) < 0) {
		log_sys_error("lseek", dev->name);
		return 0;
	}

	r = _write(fd, buffer, len);
	close(fd);
	return r;
}
