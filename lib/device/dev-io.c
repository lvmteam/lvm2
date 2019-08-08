/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/device/device.h"
#include "lib/metadata/metadata.h"
#include "lib/mm/memlock.h"

#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef __linux__
#  define u64 uint64_t		/* Missing without __KERNEL__ */
#  undef WNOHANG		/* Avoid redefinition */
#  undef WUNTRACED		/* Avoid redefinition */
#  include <linux/fs.h>		/* For block ioctl definitions */
#  define BLKSIZE_SHIFT SECTOR_SHIFT
#  ifndef BLKGETSIZE64		/* fs.h out-of-date */
#    define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#  endif /* BLKGETSIZE64 */
#  ifndef BLKDISCARD
#    define BLKDISCARD	_IO(0x12,119)
#  endif
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

static unsigned _dev_size_seqno = 1;

static int _dev_get_size_file(struct device *dev, uint64_t *size)
{
	const char *name = dev_name(dev);
	struct stat info;

	if (dev->size_seqno == _dev_size_seqno) {
		log_very_verbose("%s: using cached size %" PRIu64 " sectors",
				 name, dev->size);
		*size = dev->size;
		return 1;
	}

	if (stat(name, &info)) {
		log_sys_error("stat", name);
		return 0;
	}

	*size = info.st_size;
	*size >>= SECTOR_SHIFT;	/* Convert to sectors */
	dev->size = *size;
	dev->size_seqno = _dev_size_seqno;

	log_very_verbose("%s: size is %" PRIu64 " sectors", name, *size);

	return 1;
}

static int _dev_get_size_dev(struct device *dev, uint64_t *size)
{
	const char *name = dev_name(dev);
	int fd = dev->bcache_fd;
	int do_close = 0;

	if (dev->size_seqno == _dev_size_seqno) {
		log_very_verbose("%s: using cached size %" PRIu64 " sectors",
				 name, dev->size);
		*size = dev->size;
		return 1;
	}

	if (fd <= 0) {
		if (!dev_open_readonly(dev))
			return_0;
		fd = dev_fd(dev);
		do_close = 1;
	}

	if (ioctl(fd, BLKGETSIZE64, size) < 0) {
		log_sys_error("ioctl BLKGETSIZE64", name);
		if (do_close && !dev_close_immediate(dev))
			log_sys_error("close", name);
		return 0;
	}

	*size >>= BLKSIZE_SHIFT;	/* Convert to sectors */
	dev->size = *size;
	dev->size_seqno = _dev_size_seqno;

	log_very_verbose("%s: size is %" PRIu64 " sectors", name, *size);

	if (do_close && !dev_close_immediate(dev))
		log_sys_error("close", name);

	return 1;
}

static int _dev_read_ahead_dev(struct device *dev, uint32_t *read_ahead)
{
	long read_ahead_long;

	if (dev->read_ahead != -1) {
		*read_ahead = (uint32_t) dev->read_ahead;
		return 1;
	}

	if (!dev_open_readonly(dev))
		return_0;

	if (ioctl(dev->fd, BLKRAGET, &read_ahead_long) < 0) {
		log_sys_error("ioctl BLKRAGET", dev_name(dev));
		if (!dev_close_immediate(dev))
			stack;
		return 0;
	}

	*read_ahead = (uint32_t) read_ahead_long;
	dev->read_ahead = read_ahead_long;

	log_very_verbose("%s: read_ahead is %u sectors",
			 dev_name(dev), *read_ahead);

	if (!dev_close_immediate(dev))
		stack;

	return 1;
}

static int _dev_discard_blocks(struct device *dev, uint64_t offset_bytes, uint64_t size_bytes)
{
	uint64_t discard_range[2];

	if (!dev_open(dev))
		return_0;

	discard_range[0] = offset_bytes;
	discard_range[1] = size_bytes;

	log_debug_devs("Discarding %" PRIu64 " bytes offset %" PRIu64 " bytes on %s. %s",
		       size_bytes, offset_bytes, dev_name(dev),
		       test_mode() ? " (test mode - suppressed)" : "");

	if (!test_mode() && ioctl(dev->fd, BLKDISCARD, &discard_range) < 0) {
		log_error("%s: BLKDISCARD ioctl at offset %" PRIu64 " size %" PRIu64 " failed: %s.",
			  dev_name(dev), offset_bytes, size_bytes, strerror(errno));
		if (!dev_close_immediate(dev))
			stack;
		/* It doesn't matter if discard failed, so return success. */
		return 1;
	}

	if (!dev_close_immediate(dev))
		stack;

	return 1;
}

int dev_get_direct_block_sizes(struct device *dev, unsigned int *physical_block_size,
				unsigned int *logical_block_size)
{
	int fd = dev->bcache_fd;
	int do_close = 0;
	unsigned int pbs = 0;
	unsigned int lbs = 0;

	if (dev->physical_block_size || dev->logical_block_size) {
		*physical_block_size = dev->physical_block_size;
		*logical_block_size = dev->logical_block_size;
		return 1;
	}

	if (fd <= 0) {
		if (!dev_open_readonly(dev))
			return 0;
		fd = dev_fd(dev);
		do_close = 1;
	}

#ifdef BLKPBSZGET /* not defined before kernel version 2.6.32 (e.g. rhel5) */
	/*
	 * BLKPBSZGET from kernel comment for blk_queue_physical_block_size:
	 * "the lowest possible sector size that the hardware can operate on
	 * without reverting to read-modify-write operations"
	 */
	if (ioctl(fd, BLKPBSZGET, &pbs)) {
		stack;
		pbs = 0;
	}
#endif

	/*
	 * BLKSSZGET from kernel comment for blk_queue_logical_block_size:
	 * "the lowest possible block size that the storage device can address."
	 */
	if (ioctl(fd, BLKSSZGET, &lbs)) {
		stack;
		lbs = 0;
	}

	dev->physical_block_size = pbs;
	dev->logical_block_size = lbs;

	*physical_block_size = pbs;
	*logical_block_size = lbs;

	if (do_close && !dev_close_immediate(dev))
		stack;

	return 1;
}

/*-----------------------------------------------------------------
 * Public functions
 *---------------------------------------------------------------*/
void dev_size_seqno_inc(void)
{
	_dev_size_seqno++;
}

int dev_get_size(struct device *dev, uint64_t *size)
{
	if (!dev)
		return 0;

	if ((dev->flags & DEV_REGULAR))
		return _dev_get_size_file(dev, size);

	return _dev_get_size_dev(dev, size);
}

int dev_get_read_ahead(struct device *dev, uint32_t *read_ahead)
{
	if (!dev)
		return 0;

	if (dev->flags & DEV_REGULAR) {
		*read_ahead = 0;
		return 1;
	}

	return _dev_read_ahead_dev(dev, read_ahead);
}

int dev_discard_blocks(struct device *dev, uint64_t offset_bytes, uint64_t size_bytes)
{
	if (!dev)
		return 0;

	if (dev->flags & DEV_REGULAR)
		return 1;

	return _dev_discard_blocks(dev, offset_bytes, size_bytes);
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
	int need_excl = 0, need_rw = 0;

	if ((flags & O_ACCMODE) == O_RDWR)
		need_rw = 1;

	if ((flags & O_EXCL))
		need_excl = 1;

	if (dev->fd >= 0) {
		if (((dev->flags & DEV_OPENED_RW) || !need_rw) &&
		    ((dev->flags & DEV_OPENED_EXCL) || !need_excl)) {
			dev->open_count++;
			return 1;
		}

		if (dev->open_count && !need_excl)
			log_debug_devs("%s: Already opened read-only. Upgrading "
				       "to read-write.", dev_name(dev));

		/* dev_close_immediate will decrement this */
		dev->open_count++;

		if (!dev_close_immediate(dev))
			return_0;
		// FIXME: dev with DEV_ALLOCED is released
		// but code is referencing it
	}

	if (critical_section())
		/* FIXME Make this log_error */
		log_verbose("dev_open(%s) called while suspended",
			    dev_name(dev));

	if (!(name = dev_name_confirmed(dev, quiet)))
		return_0;

#ifdef O_DIRECT_SUPPORT
	if (direct) {
		if (!(dev->flags & DEV_O_DIRECT_TESTED))
			dev->flags |= DEV_O_DIRECT;

		if ((dev->flags & DEV_O_DIRECT))
			flags |= O_DIRECT;
	}
#endif

#ifdef O_NOATIME
	/* Don't update atime on device inodes */
	if (!(dev->flags & DEV_REGULAR) && !(dev->flags & DEV_NOT_O_NOATIME))
		flags |= O_NOATIME;
#endif

	if ((dev->fd = open(name, flags, 0777)) < 0) {
#ifdef O_NOATIME
		if ((errno == EPERM) && (flags & O_NOATIME)) {
			flags &= ~O_NOATIME;
			dev->flags |= DEV_NOT_O_NOATIME;
			if ((dev->fd = open(name, flags, 0777)) >= 0) {
				log_debug_devs("%s: Not using O_NOATIME", name);
				goto opened;
			}
		}
#endif

#ifdef O_DIRECT_SUPPORT
		if (direct && !(dev->flags & DEV_O_DIRECT_TESTED)) {
			flags &= ~O_DIRECT;
			if ((dev->fd = open(name, flags, 0777)) >= 0) {
				dev->flags &= ~DEV_O_DIRECT;
				log_debug_devs("%s: Not using O_DIRECT", name);
				goto opened;
			}
		}
#endif
		if (quiet)
			log_sys_debug("open", name);
		else
			log_sys_error("open", name);

		dev->flags |= DEV_OPEN_FAILURE;
		return 0;
	}

#ifdef O_DIRECT_SUPPORT
      opened:
	if (direct)
		dev->flags |= DEV_O_DIRECT_TESTED;
#endif
	dev->open_count++;

	if (need_rw)
		dev->flags |= DEV_OPENED_RW;
	else
		dev->flags &= ~DEV_OPENED_RW;

	if (need_excl)
		dev->flags |= DEV_OPENED_EXCL;
	else
		dev->flags &= ~DEV_OPENED_EXCL;

	if (!(dev->flags & DEV_REGULAR) &&
	    ((fstat(dev->fd, &buf) < 0) || (buf.st_rdev != dev->dev))) {
		log_error("%s: fstat failed: Has device name changed?", name);
		if (!dev_close_immediate(dev))
			stack;
		return 0;
	}

#ifndef O_DIRECT_SUPPORT
	if (!(dev->flags & DEV_REGULAR))
		dev_flush(dev);
#endif

	if ((flags & O_CREAT) && !(flags & O_TRUNC))
		dev->end = lseek(dev->fd, (off_t) 0, SEEK_END);

	log_debug_devs("Opened %s %s%s%s", dev_name(dev),
		       dev->flags & DEV_OPENED_RW ? "RW" : "RO",
		       dev->flags & DEV_OPENED_EXCL ? " O_EXCL" : "",
		       dev->flags & DEV_O_DIRECT ? " O_DIRECT" : "");

	dev->flags &= ~DEV_OPEN_FAILURE;
	return 1;
}

int dev_open_quiet(struct device *dev)
{
	return dev_open_flags(dev, O_RDWR, 1, 1);
}

int dev_open(struct device *dev)
{
	return dev_open_flags(dev, O_RDWR, 1, 0);
}

int dev_open_readonly(struct device *dev)
{
	return dev_open_flags(dev, O_RDONLY, 1, 0);
}

int dev_open_readonly_buffered(struct device *dev)
{
	return dev_open_flags(dev, O_RDONLY, 0, 0);
}

int dev_open_readonly_quiet(struct device *dev)
{
	return dev_open_flags(dev, O_RDONLY, 1, 1);
}

static void _close(struct device *dev)
{
	if (close(dev->fd))
		log_sys_error("close", dev_name(dev));
	dev->fd = -1;

	log_debug_devs("Closed %s", dev_name(dev));

	if (dev->flags & DEV_ALLOCED)
		dev_destroy_file(dev);
}

static int _dev_close(struct device *dev, int immediate)
{
	if (dev->fd < 0) {
		log_error("Attempt to close device '%s' "
			  "which is not open.", dev_name(dev));
		return 0;
	}

	if (dev->open_count > 0)
		dev->open_count--;

	if (immediate && dev->open_count)
		log_debug_devs("%s: Immediate close attempt while still referenced",
			       dev_name(dev));

	if (immediate || (dev->open_count < 1))
		_close(dev);

	return 1;
}

int dev_close(struct device *dev)
{
	return _dev_close(dev, 0);
}

int dev_close_immediate(struct device *dev)
{
	return _dev_close(dev, 1);
}
