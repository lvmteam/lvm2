/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "device.h"
#include "lvm-types.h"
#include "metadata.h"

#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>		// UGH!!! for BLKSSZGET

/* FIXME 64 bit offset!!!
_syscall5(int,  _llseek,  uint,  fd, ulong, hi, ulong, lo, loff_t *, res, uint, wh);
*/

int dev_get_size(struct device *dev, uint64_t *size)
{
	int fd;
	long s;
	const char *name = dev_name(dev);

	log_very_verbose("Getting size of %s", name);
	if ((fd = open(name, O_RDONLY)) < 0) {
		log_sys_error("open", name);
		return 0;
	}

	/* FIXME: add 64 bit ioctl */
	if (ioctl(fd, BLKGETSIZE, &s) < 0) {
		log_sys_error("ioctl BLKGETSIZE", name);
		close(fd);
		return 0;
	}

	close(fd);
	*size = (uint64_t) s;
	return 1;
}

int dev_get_sectsize(struct device *dev, uint32_t *size)
{
	int fd;
	int s;
	const char *name = dev_name(dev);

	log_very_verbose("Getting size of %s", name);
	if ((fd = open(name, O_RDONLY)) < 0) {
		log_sys_error("open", name);
		return 0;
	}

	if (ioctl(fd, BLKSSZGET, &s) < 0) {
		log_sys_error("ioctl BLKSSZGET", name);
		close(fd);
		return 0;
	}

	close(fd);
	*size = (uint32_t) s;
	return 1;
}

static void _flush(int fd)
{
	ioctl(fd, BLKFLSBUF, 0);
}

int dev_open(struct device *dev, int flags)
{
	struct stat buf;
	const char *name = dev_name_confirmed(dev);

	if (!name) {
		stack;
		return 0;
	}

	if (dev->fd >= 0) {
		log_error("Device '%s' has already been opened", name);
		return 0;
	}

	if ((stat(name, &buf) < 0) || (buf.st_rdev != dev->dev)) {
		log_error("%s: stat failed: Has device name changed?", name);
		return 0;
	}

	if ((dev->fd = open(name, flags)) < 0) {
		log_sys_error("open", name);
		return 0;
	}

	if ((fstat(dev->fd, &buf) < 0) || (buf.st_rdev != dev->dev)) {
		log_error("%s: fstat failed: Has device name changed?", name);
		dev_close(dev);
		dev->fd = -1;
		return 0;
	}
	_flush(dev->fd);
	dev->flags = 0;

	return 1;
}

int dev_close(struct device *dev)
{
	if (dev->fd < 0) {
		log_error("Attempt to close device '%s' "
			  "which is not open.", dev_name(dev));
		return 0;
	}

	if (dev->flags & DEV_ACCESSED_W)
		_flush(dev->fd);

	if (close(dev->fd))
		log_sys_error("close", dev_name(dev));

	dev->fd = -1;

	return 1;
}

ssize_t raw_read(int fd, void *buf, size_t count)
{
	ssize_t n = 0, tot = 0;

	if (count > SSIZE_MAX)
		return -1;

	while (tot < (signed) count) {
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

ssize_t dev_read(struct device *dev, uint64_t offset, size_t len, void *buffer)
{
	const char *name = dev_name(dev);
	int fd = dev->fd;
	/* loff_t pos; */

	if (fd < 0) {
		log_err("Attempt to read an unopened device (%s).", name);
		return 0;
	}

	/* if (_llseek((unsigned) fd, (ulong) (offset >> 32), (ulong) (offset & 0xFFFFFFFF), &pos, SEEK_SET) < 0) { */
	if (lseek(fd, (off_t) offset, SEEK_SET) < 0) {
		log_sys_error("lseek", name);
		return 0;
	}

	return raw_read(fd, buffer, len);
}

static int _write(int fd, const void *buf, size_t count)
{
	ssize_t n = 0;
	int tot = 0;

	/* Skip all writes */
	if (test_mode())
		return count;

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

int64_t dev_write(struct device * dev, uint64_t offset, size_t len,
		  void *buffer)
{
	const char *name = dev_name(dev);
	int fd = dev->fd;

	if (fd < 0) {
		log_error("Attempt to write to unopened device %s", name);
		return 0;
	}

	if (lseek(fd, (off_t) offset, SEEK_SET) < 0) {
		log_sys_error("lseek", name);
		return 0;
	}

	dev->flags |= DEV_ACCESSED_W;

	return _write(fd, buffer, len);
}

int dev_zero(struct device *dev, uint64_t offset, size_t len)
{
	int64_t r;
	size_t s;
	char buffer[4096];
	int already_open;

	already_open = dev_is_open(dev);

	if (!already_open && !dev_open(dev, O_RDWR)) {
		stack;
		return 0;
	}

	if (lseek(dev->fd, (off_t) offset, SEEK_SET) < 0) {
		log_sys_error("lseek", dev_name(dev));
		if (!already_open && !dev_close(dev))
			stack;
		return 0;
	}

	if ((offset % SECTOR_SIZE) || (len % SECTOR_SIZE))
		log_debug("Wiping %s at %" PRIu64 " length %" PRIsize_t,
			  dev_name(dev), offset, len);
	else
		log_debug("Wiping %s at sector %" PRIu64 " length %" PRIsize_t
			  " sectors", dev_name(dev), offset >> SECTOR_SHIFT,
			  len >> SECTOR_SHIFT);

	memset(buffer, 0, sizeof(buffer));
	while (1) {
		s = len > sizeof(buffer) ? sizeof(buffer) : len;
		r = _write(dev->fd, buffer, s);

		if (r <= 0)
			break;

		len -= r;
		if (!len) {
			r = 1;
			break;
		}
	}

	dev->flags |= DEV_ACCESSED_W;

	if (!already_open && !dev_close(dev))
		stack;

	/* FIXME: Always display error */
	return (len == 0);
}
