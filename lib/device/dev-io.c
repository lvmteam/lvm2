/*
 * Copyright (C) 2002 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "device.h"
#include "lvm-types.h"
#include "log.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>		/* UGH!!! for BLKSSZGET */


/* Buffer for O_DIRECT, allocated as
   twice _SC_PAGE_SIZE so we can make sure it is
   properly aligned.
   It is assumed that _SC_PAGE_SIZE is a power of 2
   but I think that's fairly safe :)
*/
static char *bigbuf;
static char *aligned_buf;
static long  page_size;

int dev_get_size(struct device *dev, uint64_t * size)
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

int dev_get_sectsize(struct device *dev, uint32_t * size)
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

int dev_open(struct device *dev, int flags)
{
	struct stat buf;
	const char *name = dev_name_confirmed(dev);

	/* First time through - allocate & align the buffer */
	if (!page_size) {
	    page_size = sysconf(_SC_PAGESIZE);
	    bigbuf = malloc(page_size*2);
	    if (!bigbuf) {
		stack;
		return 0;
	    }
	    aligned_buf = (char *)(((long)bigbuf + page_size) & ~(page_size-1) );
	}

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

	/* If write was requested then add read so we can pre-fill
	   buffers for aligned writes */
	if (flags & O_WRONLY)
	    flags = (flags & ~O_WRONLY) | O_RDWR;

	if ((dev->fd = open(name, flags|O_DIRECT)) < 0) {
		log_sys_error("open", name);
		return 0;
	}

	if ((fstat(dev->fd, &buf) < 0) || (buf.st_rdev != dev->dev)) {
		log_error("%s: fstat failed: Has device name changed?", name);
		dev_close(dev);
		return 0;
	}

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

	if (close(dev->fd))
		log_sys_error("close", dev_name(dev));

	dev->fd = -1;

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

int64_t dev_read(struct device * dev, uint64_t offset,
		 int64_t len, void *buffer)
{
	const char *name = dev_name(dev);
	int fd = dev->fd;
	int64_t newlen;
	int64_t newoffset;
	int64_t offsetdiff;
	int ret;

	if (fd < 0) {
		log_err("Attempt to read an unopened device (%s).", name);
		return 0;
	}

	/* Adjust offset to page size */
	newoffset = offset & ~(page_size-1);
	offsetdiff = offset - newoffset;

	if (lseek(fd, newoffset, SEEK_SET) < 0) {
		log_sys_error("lseek", name);
		return 0;
	}

        /* Copy to aligned buffer & pad to page size */
	newlen = (len+offsetdiff) + page_size - ((len+offsetdiff) % page_size);

	ret =  _read(fd, aligned_buf, newlen);
	if (ret < len+offsetdiff)
	{
	    return ret;
	}

	/* Copy back to users buffer */
	memcpy(buffer, aligned_buf+offsetdiff, len);
	return len;
}

int _write(int fd, const void *buf, size_t count)
{
	size_t n = 0;
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

int64_t dev_write(struct device * dev, uint64_t offset,
		  int64_t len, void *buffer)
{
	const char *name = dev_name(dev);
	int fd = dev->fd;
	int64_t newlen;
	int64_t newoffset;
	int64_t offsetdiff;
	int ret;

	if (fd < 0) {
		log_error("Attempt to write to unopened device %s", name);
		return 0;
	}

	/* Adjust offset to page size */
	newoffset = offset & ~(page_size-1);
	offsetdiff = offset - newoffset;

	if (lseek(fd, newoffset, SEEK_SET) < 0) {
		log_sys_error("lseek", name);
		return 0;
	}

	dev->flags |= DEV_ACCESSED_W;

        /* Pad to page size */
	newlen = (len+offsetdiff) + page_size - ((len+offsetdiff) % page_size);

	/* We read the page in and just overwrite the bits
	   requested. Needs to fudge O_RDWR in dev_open for this to work. */
	if (_read(fd, aligned_buf, newlen) <= 0) {
		log_sys_error("pre-read", name);
		return 0;
	}
	if (lseek(fd, newoffset, SEEK_SET) < 0) {
		log_sys_error("re-lseek", name);
		return 0;
	}

	memcpy(aligned_buf+offsetdiff, buffer, len);
	ret = _write(fd, aligned_buf, newlen);

	if (ret == newlen)
	        return len;
	else
	        return ret;
}

int dev_zero(struct device *dev, uint64_t offset, int64_t len)
{
	int64_t r, s;
	const char *name = dev_name(dev);
	int fd = dev->fd;

	if (fd < 0) {
		log_error("Attempt to zero part of an unopened device %s",
			  name);
		return 0;
	}

	if (lseek(fd, offset, SEEK_SET) < 0) {
		log_sys_error("lseek", name);
		return 0;
	}

	memset(aligned_buf, 0, page_size);
	while (1) {
		s = len > page_size ? page_size : len;
		r = _write(fd, aligned_buf, s);

		if (r <= 0)
			break;

		len -= r;
		if (!len) {
			r = 1;
			break;
		}
	}

	dev->flags |= DEV_ACCESSED_W;

	/* FIXME: Always display error */
	return (len == 0);
}
