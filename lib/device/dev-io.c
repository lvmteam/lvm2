/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "lvm-types.h"
#include "device.h"
#include "metadata.h"
#include "lvmcache.h"
#include "memlock.h"

#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef linux
#  define u64 uint64_t		/* Missing without __KERNEL__ */
#  undef WNOHANG		/* Avoid redefinition */
#  undef WUNTRACED		/* Avoid redefinition */
#  include <linux/fs.h>		/* For block ioctl definitions */
#  define BLKSIZE_SHIFT SECTOR_SHIFT
#else
#  include <sys/disk.h>
#  define BLKBSZGET DKIOCGETBLOCKSIZE
#  define BLKSSZGET DKIOCGETBLOCKSIZE
#  define BLKGETSIZE64 DKIOCGETBLOCKCOUNT
#  define BLKFLSBUF DKIOCSYNCHRONIZECACHE
#  define BLKSIZE_SHIFT 0
#endif

#ifdef O_DIRECT_SUPPORT
#  ifndef O_DIRECT
#    error O_DIRECT support configured but O_DIRECT definition not found in headers
#  endif
#endif


/* FIXME Use _llseek for 64-bit
_syscall5(int,  _llseek,  uint,  fd, ulong, hi, ulong, lo, loff_t *, res, uint, wh);
 if (_llseek((unsigned) fd, (ulong) (offset >> 32), (ulong) (offset & 0xFFFFFFFF), &pos, SEEK_SET) < 0) { 
*/

static LIST_INIT(_open_devices);

/*-----------------------------------------------------------------
 * The standard io loop that keeps submitting an io until it's
 * all gone.
 *---------------------------------------------------------------*/
static int _io(struct device_area *where, void *buffer, int should_write)
{
	int fd = dev_fd(where->dev);
	ssize_t n = 0;
	size_t total = 0;

	if (fd < 0) {
		log_error("Attempt to read an unopened device (%s).",
			  dev_name(where->dev));
		return 0;
	}

	/*
	 * Skip all writes in test mode.
	 */
	if (should_write && test_mode())
		return 1;

	if (where->size > SSIZE_MAX) {
		log_error("Read size too large: %" PRIu64, where->size);
		return 0;
	}

	if (lseek(fd, (off_t) where->start, SEEK_SET) < 0) {
		log_sys_error("lseek", dev_name(where->dev));
		return 0;
	}

	while (total < (size_t) where->size) {
		do
			n = should_write ?
			    write(fd, buffer, (size_t) where->size - total) :
			    read(fd, buffer, (size_t) where->size - total);
		while ((n < 0) && ((errno == EINTR) || (errno == EAGAIN)));

		if (n <= 0)
			break;

		total += n;
		buffer += n;
	}

	return (total == (size_t) where->size);
}

/*-----------------------------------------------------------------
 * LVM2 uses O_DIRECT when performing metadata io, which requires
 * block size aligned accesses.  If any io is not aligned we have
 * to perform the io via a bounce buffer, obviously this is quite
 * inefficient.
 *---------------------------------------------------------------*/

/*
 * Get the sector size from an _open_ device.
 */
static int _get_block_size(struct device *dev, unsigned int *size)
{
	int s;

	if (ioctl(dev_fd(dev), BLKBSZGET, &s) < 0) {
		log_sys_error("ioctl BLKBSZGET", dev_name(dev));
		return 0;
	}

	*size = (unsigned int) s;
	return 1;
}

/*
 * Widens a region to be an aligned region.
 */
static void _widen_region(unsigned int block_size, struct device_area *region,
			  struct device_area *result)
{
	uint64_t mask = block_size - 1, delta;
	memcpy(result, region, sizeof(*result));

	/* adjust the start */
	delta = result->start & mask;
	if (delta) {
		result->start -= delta;
		result->size += delta;
	}

	/* adjust the end */
	delta = (result->start + result->size) & mask;
	if (delta)
		result->size += block_size - delta;
}

static int _aligned_io(struct device_area *where, void *buffer,
		       int should_write)
{
	void *bounce;
	unsigned int block_size = 0;
	uintptr_t mask;
	struct device_area widened;

	if (!(where->dev->flags & DEV_REGULAR) &&
	    !_get_block_size(where->dev, &block_size)) {
		stack;
		return 0;
	}

	if (!block_size)
		block_size = SECTOR_SIZE * 2;

	_widen_region(block_size, where, &widened);

	/* Do we need to use a bounce buffer? */
	mask = block_size - 1;
	if (!memcmp(where, &widened, sizeof(widened)) &&
	    !((uintptr_t) buffer & mask))
		return _io(where, buffer, should_write);

	/* Allocate a bounce buffer with an extra block */
	if (!(bounce = alloca((size_t) widened.size + block_size))) {
		log_error("Bounce buffer alloca failed");
		return 0;
	}

	/*
	 * Realign start of bounce buffer (using the extra sector)
	 */
	if (((uintptr_t) bounce) & mask)
		bounce = (void *) ((((uintptr_t) bounce) + mask) & ~mask);

	/* channel the io through the bounce buffer */
	if (!_io(&widened, bounce, 0)) {
		if (!should_write) {
			stack;
			return 0;
		}
		/* FIXME pre-extend the file */
		memset(bounce, '\n', widened.size);
	}

	if (should_write) {
		memcpy(bounce + (where->start - widened.start), buffer,
		       (size_t) where->size);

		/* ... then we write */
		return _io(&widened, bounce, 1);
	}

	memcpy(buffer, bounce + (where->start - widened.start),
	       (size_t) where->size);

	return 1;
}

/*-----------------------------------------------------------------
 * Public functions
 *---------------------------------------------------------------*/

int dev_get_size(struct device *dev, uint64_t *size)
{
	int fd;
	const char *name = dev_name(dev);

	log_very_verbose("Getting size of %s", name);
	if ((fd = open(name, O_RDONLY)) < 0) {
		log_sys_error("open", name);
		return 0;
	}

	if (ioctl(fd, BLKGETSIZE64, size) < 0) {
		log_sys_error("ioctl BLKGETSIZE64", name);
		close(fd);
		return 0;
	}

	*size >>= BLKSIZE_SHIFT;	/* Convert to sectors */
	close(fd);
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

void dev_flush(struct device *dev)
{
	if (!(dev->flags & DEV_REGULAR) && ioctl(dev->fd, BLKFLSBUF, 0) >= 0)
		return;

	if (fsync(dev->fd) >= 0)
		return;

	sync();
}

int dev_open_flags(struct device *dev, int flags, int direct, int quiet)
{
	struct stat buf;
	const char *name;

	if (dev->fd >= 0) {
		dev->open_count++;
		return 1;
	}

	if (memlock())
		log_error("WARNING: dev_open(%s) called while suspended",
			  dev_name(dev));

	if (dev->flags & DEV_REGULAR)
		name = dev_name(dev);
	else if (!(name = dev_name_confirmed(dev, quiet))) {
		stack;
		return 0;
	}

	if (!(dev->flags & DEV_REGULAR) &&
	    ((stat(name, &buf) < 0) || (buf.st_rdev != dev->dev))) {
		log_error("%s: stat failed: Has device name changed?", name);
		return 0;
	}

#ifdef O_DIRECT_SUPPORT
	if (direct)
		flags |= O_DIRECT;
#endif

	if ((dev->fd = open(name, flags, 0777)) < 0) {
		log_sys_error("open", name);
		return 0;
	}

	dev->open_count = 1;
	dev->flags &= ~DEV_ACCESSED_W;

	if (!(dev->flags & DEV_REGULAR) &&
	    ((fstat(dev->fd, &buf) < 0) || (buf.st_rdev != dev->dev))) {
		log_error("%s: fstat failed: Has device name changed?", name);
		dev_close(dev);
		dev->fd = -1;
		return 0;
	}

#ifndef O_DIRECT_SUPPORT
	if (!(dev->flags & DEV_REGULAR))
		dev_flush(dev);
#endif

	if ((flags & O_CREAT) && !(flags & O_TRUNC)) {
		dev->end = lseek(dev->fd, (off_t) 0, SEEK_END);
	}

	list_add(&_open_devices, &dev->open_list);
	log_debug("Opened %s", dev_name(dev));

	return 1;
}

int dev_open_quiet(struct device *dev)
{
	/* FIXME Open O_RDONLY if vg read lock? */
	return dev_open_flags(dev, O_RDWR, 1, 1);
}

int dev_open(struct device *dev)
{
	/* FIXME Open O_RDONLY if vg read lock? */
	return dev_open_flags(dev, O_RDWR, 1, 0);
}

static void _close(struct device *dev)
{
	if (close(dev->fd))
		log_sys_error("close", dev_name(dev));
	dev->fd = -1;
	list_del(&dev->open_list);

	log_debug("Closed %s", dev_name(dev));

	if (dev->flags & DEV_ALLOCED) {
		dbg_free((void *) list_item(dev->aliases.n, struct str_list)->
			 str);
		dbg_free(dev->aliases.n);
		dbg_free(dev);
	}
}

int dev_close(struct device *dev)
{
	if (dev->fd < 0) {
		log_error("Attempt to close device '%s' "
			  "which is not open.", dev_name(dev));
		return 0;
	}

#ifndef O_DIRECT_SUPPORT
	if (dev->flags & DEV_ACCESSED_W)
		dev_flush(dev);
#endif

	/* FIXME lookup device in cache to get vgname and see if it's locked? */
	if (--dev->open_count < 1 && !vgs_locked())
		_close(dev);

	return 1;
}

void dev_close_all(void)
{
	struct list *doh, *doht;
	struct device *dev;

	list_iterate_safe(doh, doht, &_open_devices) {
		dev = list_struct_base(doh, struct device, open_list);
		if (dev->open_count < 1)
			_close(dev);
	}
}

int dev_read(struct device *dev, uint64_t offset, size_t len, void *buffer)
{
	struct device_area where;

	if (!dev->open_count)
		return 0;

	where.dev = dev;
	where.start = offset;
	where.size = len;

	return _aligned_io(&where, buffer, 0);
}

/* FIXME If O_DIRECT can't extend file, dev_extend first; dev_truncate after.
 *       But fails if concurrent processes writing
 */

/* FIXME pre-extend the file */
int dev_append(struct device *dev, size_t len, void *buffer)
{
	int r;

	if (!dev->open_count)
		return 0;

	r = dev_write(dev, dev->end, len, buffer);
	dev->end += (uint64_t) len;

#ifndef O_DIRECT_SUPPORT
	dev_flush(dev);
#endif
	return r;
}

int dev_write(struct device *dev, uint64_t offset, size_t len, void *buffer)
{
	struct device_area where;

	if (!dev->open_count)
		return 0;

	where.dev = dev;
	where.start = offset;
	where.size = len;

	dev->flags |= DEV_ACCESSED_W;

	return _aligned_io(&where, buffer, 1);
}

int dev_zero(struct device *dev, uint64_t offset, size_t len)
{
	size_t s;
	char buffer[4096];

	if (!dev_open(dev)) {
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
		if (!dev_write(dev, offset, s, buffer))
			break;

		len -= s;
		if (!len)
			break;

		offset += s;
	}

	dev->flags |= DEV_ACCESSED_W;

	if (!dev_close(dev))
		stack;

	/* FIXME: Always display error */
	return (len == 0);
}
