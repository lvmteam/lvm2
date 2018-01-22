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

#include "lib.h"
#include "device.h"
#include "metadata.h"
#include "lvmcache.h"
#include "memlock.h"
#include "locking.h"

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

/*
 * Always read at least 8k from disk.
 * This seems to be a good compromise for the existing LVM2 metadata layout.
 */
#define MIN_READ_SIZE (8 * 1024)

static DM_LIST_INIT(_open_devices);
static unsigned _dev_size_seqno = 1;

static const char *_reasons[] = {
	"dev signatures",
	"PV labels",
	"VG metadata header",
	"VG metadata content",
	"extra VG metadata header",
	"extra VG metadata content",
	"LVM1 metadata",
	"pool metadata",
	"LV content",
	"logging",
};

static const char *_reason_text(dev_io_reason_t reason)
{
	return _reasons[(unsigned) reason];
}

/*
 * Release the memory holding the last data we read
 */
static void _release_devbuf(struct device_buffer *devbuf)
{
	dm_free(devbuf->malloc_address);
	devbuf->malloc_address = NULL;
}

void devbufs_release(struct device *dev)
{
	if ((dev->flags & DEV_REGULAR))
		return;

	_release_devbuf(&dev->last_devbuf);
	_release_devbuf(&dev->last_extra_devbuf);
}

#ifdef AIO_SUPPORT

#  include <libaio.h>

static io_context_t _aio_ctx = 0;
static struct io_event *_aio_events = NULL;
static int _aio_max = 0;
static int64_t _aio_memory_max = 0;
static int _aio_must_queue = 0;		/* Have we reached AIO capacity? */

static DM_LIST_INIT(_aio_queue);

#define DEFAULT_AIO_COLLECTION_EVENTS 32

int dev_async_setup(struct cmd_context *cmd)
{
	int r;

	_aio_max = find_config_tree_int(cmd, devices_aio_max_CFG, NULL);
	_aio_memory_max = find_config_tree_int(cmd, devices_aio_memory_CFG, NULL) * 1024 * 1024;

	/* Threshold is zero? */
	if (!_aio_max || !_aio_memory_max) {
		if (_aio_ctx)
			dev_async_exit();
		return 1;
	}

	/* Already set up? */
	if (_aio_ctx)
		return 1;

	log_debug_io("Setting up aio context for up to %" PRId64 " MB across %d events.", _aio_memory_max, _aio_max);

	if (!_aio_events && !(_aio_events = dm_zalloc(sizeof(*_aio_events) * DEFAULT_AIO_COLLECTION_EVENTS))) {
		log_error("Failed to allocate io_event array for asynchronous I/O.");
		return 0;
	}

	if ((r = io_setup(_aio_max, &_aio_ctx)) < 0) {
		/*
		 * Possible errors:
		 *   ENOSYS - aio not available in current kernel
		 *   EAGAIN - _aio_max is too big
		 *   EFAULT - invalid pointer
		 *   EINVAL - _aio_ctx != 0 or kernel aio limits exceeded
		 *   ENOMEM
		 */
		log_warn("WARNING: Asynchronous I/O setup for %d events failed: %s", _aio_max, strerror(-r));
		log_warn("WARNING: Using only synchronous I/O.");
		dm_free(_aio_events);
		_aio_events = NULL;
		_aio_ctx = 0;
		return 0;
	}

	return 1;
}

/* Reset aio context after fork */
int dev_async_reset(struct cmd_context *cmd)
{
	log_debug_io("Resetting asynchronous I/O context.");
	_aio_ctx = 0;
	dm_free(_aio_events);
	_aio_events = NULL;

	return dev_async_setup(cmd);
}

/*
 * Track the amount of in-flight async I/O.
 * If it exceeds the defined threshold set _aio_must_queue.
 */
static void _update_aio_counters(int nr, ssize_t bytes)
{
	static int64_t aio_bytes = 0;
	static int aio_count = 0;

	aio_bytes += bytes;
	aio_count += nr;

	if (aio_count >= _aio_max || aio_bytes > _aio_memory_max)
		_aio_must_queue = 1;
	else
		_aio_must_queue = 0;
}

static int _io(struct device_buffer *devbuf, unsigned ioflags);

int dev_async_getevents(void)
{
	struct device_buffer *devbuf, *tmp;
	lvm_callback_fn_t dev_read_callback_fn;
	void *dev_read_callback_context;
	int r, event_nr;

	if (!_aio_ctx)
		return 1;

	do {
		/* FIXME Add timeout - currently NULL - waits for ever for at least 1 item */
		r = io_getevents(_aio_ctx, 1, DEFAULT_AIO_COLLECTION_EVENTS, _aio_events, NULL);
		if (r > 0)
			break;
		if (!r)
			return 1; /* Timeout elapsed */
		if (r == -EINTR)
			continue;
		if (r == -EAGAIN) {
			usleep(100);
			return 1; /* Give the caller the opportunity to do other work before repeating */
		}
		/*
		 * ENOSYS - not supported by kernel
		 * EFAULT - memory invalid
		 * EINVAL - _aio_ctx invalid or min_nr/nr/timeout out of range
		 */
		log_error("Asynchronous event collection failed: %s", strerror(-r));
		return 0;
	} while (1);

	for (event_nr = 0; event_nr < r; event_nr++) {
		devbuf = _aio_events[event_nr].obj->data;
		dm_free(_aio_events[event_nr].obj);

		_update_aio_counters(-1, -devbuf->where.size);

		dev_read_callback_fn = devbuf->dev_read_callback_fn;
		dev_read_callback_context = devbuf->dev_read_callback_context;

		/* Clear the callbacks as a precaution */
		devbuf->dev_read_callback_context = NULL;
		devbuf->dev_read_callback_fn = NULL;

		if (_aio_events[event_nr].res == devbuf->where.size) {
			if (dev_read_callback_fn)
				dev_read_callback_fn(0, AIO_SUPPORTED_CODE_PATH, dev_read_callback_context, (char *)devbuf->buf + devbuf->data_offset);
		} else {
			/* FIXME If partial read is possible, resubmit remainder */
			log_error_once("%s: Asynchronous I/O failed: read only %" PRIu64 " of %" PRIu64 " bytes at %" PRIu64,
				       dev_name(devbuf->where.dev),
				       (uint64_t) _aio_events[event_nr].res, (uint64_t) devbuf->where.size,
				       (uint64_t) devbuf->where.start);
			_release_devbuf(devbuf);
			if (dev_read_callback_fn)
				dev_read_callback_fn(1, AIO_SUPPORTED_CODE_PATH, dev_read_callback_context, NULL);
			r = 0;
		}
	}

	/* Submit further queued events if we can */
        dm_list_iterate_items_gen_safe(devbuf, tmp, &_aio_queue, aio_queued) {
		if (_aio_must_queue)
			break;
                dm_list_del(&devbuf->aio_queued);
		_io(devbuf, 1);
        }

	return 1;
}

static int _io_async(struct device_buffer *devbuf)
{
	struct device_area *where = &devbuf->where;
	struct iocb *iocb;
	int r;

	_update_aio_counters(1, devbuf->where.size);

	if (!(iocb = dm_malloc(sizeof(*iocb)))) {
		log_error("Failed to allocate I/O control block array for asynchronous I/O.");
		return 0;
	}

	io_prep_pread(iocb, dev_fd(where->dev), devbuf->buf, where->size, where->start);
	iocb->data = devbuf;

	do {
		r = io_submit(_aio_ctx, 1L, &iocb);
		if (r ==1)
			break;	/* Success */
		if (r == -EAGAIN) {
			/* Try to release some resources then retry */
			usleep(100);
			if (dev_async_getevents())
				return_0;
			/* FIXME Add counter/timeout so we can't get stuck here for ever */
			continue;
		}
		/*
		 * Possible errors:
		 *   EFAULT - invalid data
		 *   ENOSYS - no aio support in kernel
		 *   EBADF  - bad file descriptor in iocb
		 *   EINVAL - invalid _aio_ctx / iocb not initialised / invalid operation for this fd
		 */
		log_error("Asynchronous event submission failed: %s", strerror(-r));
		return 0;
	} while (1);

	return 1;
}

void dev_async_exit(void)
{
	struct device_buffer *devbuf, *tmp;
	lvm_callback_fn_t dev_read_callback_fn;
	void *dev_read_callback_context;
	int r;

	if (!_aio_ctx)
		return;

	/* Discard any queued requests */
        dm_list_iterate_items_gen_safe(devbuf, tmp, &_aio_queue, aio_queued) {
                dm_list_del(&devbuf->aio_queued);

		_update_aio_counters(-1, -devbuf->where.size);

		dev_read_callback_fn = devbuf->dev_read_callback_fn;
		dev_read_callback_context = devbuf->dev_read_callback_context;

		_release_devbuf(devbuf);

		if (dev_read_callback_fn)
			dev_read_callback_fn(1, AIO_SUPPORTED_CODE_PATH, dev_read_callback_context, NULL);
        }

	log_debug_io("Destroying aio context.");
	if ((r = io_destroy(_aio_ctx)) < 0)
		/* Returns -ENOSYS if aio not in kernel or -EINVAL if _aio_ctx invalid */
		log_error("Failed to destroy asynchronous I/O context: %s", strerror(-r));

	dm_free(_aio_events);
	_aio_events = NULL;

	_aio_ctx = 0;
}

static void _queue_aio(struct device_buffer *devbuf)
{
	dm_list_add(&_aio_queue, &devbuf->aio_queued);
	log_debug_io("Queueing aio.");
}

#else

static int _aio_ctx = 0;
static int _aio_must_queue = 0;

int dev_async_setup(struct cmd_context *cmd)
{
	return 1;
}

int dev_async_reset(struct cmd_context *cmd)
{
	return 1;
}

int dev_async_getevents(void)
{
	return 1;
}

void dev_async_exit(void)
{
}

static int _io_async(struct device_buffer *devbuf)
{
	return 0;
}

static void _queue_aio(struct device_buffer *devbuf)
{
}

#endif /* AIO_SUPPORT */

/*-----------------------------------------------------------------
 * The standard io loop that keeps submitting an io until it's
 * all gone.
 *---------------------------------------------------------------*/
static int _io_sync(struct device_buffer *devbuf)
{
	struct device_area *where = &devbuf->where;
	int fd = dev_fd(where->dev);
	char *buffer = devbuf->buf;
	ssize_t n = 0;
	size_t total = 0;

	if (lseek(fd, (off_t) where->start, SEEK_SET) == (off_t) -1) {
		log_error("%s: lseek %" PRIu64 " failed: %s",
			  dev_name(where->dev), (uint64_t) where->start,
			  strerror(errno));
		return 0;
	}

	while (total < (size_t) where->size) {
		do
			n = devbuf->write ?
			    write(fd, buffer, (size_t) where->size - total) :
			    read(fd, buffer, (size_t) where->size - total);
		while ((n < 0) && ((errno == EINTR) || (errno == EAGAIN)));

		if (n < 0)
			log_error_once("%s: %s failed after %" PRIu64 " of %" PRIu64
				       " at %" PRIu64 ": %s", dev_name(where->dev),
				       devbuf->write ? "write" : "read",
				       (uint64_t) total,
				       (uint64_t) where->size,
				       (uint64_t) where->start, strerror(errno));

		if (n <= 0)
			break;

		total += n;
		buffer += n;
	}

	return (total == (size_t) where->size);
}

static int _io(struct device_buffer *devbuf, unsigned ioflags)
{
	struct device_area *where = &devbuf->where;
	int fd = dev_fd(where->dev);
	int async = (!devbuf->write && _aio_ctx && aio_supported_code_path(ioflags) && devbuf->dev_read_callback_fn) ? 1 : 0;

	if (fd < 0) {
		log_error("Attempt to read an unopened device (%s).",
			  dev_name(where->dev));
		return 0;
	}

	if (!devbuf->buf && !(devbuf->malloc_address = devbuf->buf = dm_malloc_aligned((size_t) devbuf->where.size, 0))) {
		log_error("I/O buffer malloc failed");
		return 0;
	}

	log_debug_io("%s %s(fd %d):%8" PRIu64 " bytes (%ssync) at %" PRIu64 "%s (for %s)",
		     devbuf->write ? "Write" : "Read ", dev_name(where->dev), fd,
		     where->size, async ? "a" : "", (uint64_t) where->start,
		     (devbuf->write && test_mode()) ? " (test mode - suppressed)" : "", _reason_text(devbuf->reason));

	/*
	 * Skip all writes in test mode.
	 */
	if (devbuf->write && test_mode())
		return 1;

	if (where->size > SSIZE_MAX) {
		log_error("Read size too large: %" PRIu64, where->size);
		return 0;
	}

	return async ? _io_async(devbuf) : _io_sync(devbuf);
}

/*-----------------------------------------------------------------
 * LVM2 uses O_DIRECT when performing metadata io, which requires
 * block size aligned accesses.  If any io is not aligned we have
 * to perform the io via a bounce buffer, obviously this is quite
 * inefficient.
 *---------------------------------------------------------------*/

/*
 * Get the physical and logical block size for a device.
 */
int dev_get_block_size(struct device *dev, unsigned int *physical_block_size, unsigned int *block_size)
{
	const char *name = dev_name(dev);
	int needs_open;
	int r = 1;

	needs_open = (!dev->open_count && (dev->phys_block_size == -1 || dev->block_size == -1));

	if (needs_open && !dev_open_readonly(dev))
		return_0;

	if (dev->block_size == -1) {
		if (ioctl(dev_fd(dev), BLKBSZGET, &dev->block_size) < 0) {
			log_sys_error("ioctl BLKBSZGET", name);
			r = 0;
			goto out;
		}
		log_debug_devs("%s: Block size is %u bytes", name, dev->block_size);
	}

#ifdef BLKPBSZGET
	/* BLKPBSZGET is available in kernel >= 2.6.32 only */
	if (dev->phys_block_size == -1) {
		if (ioctl(dev_fd(dev), BLKPBSZGET, &dev->phys_block_size) < 0) {
			log_sys_error("ioctl BLKPBSZGET", name);
			r = 0;
			goto out;
		}
		log_debug_devs("%s: Physical block size is %u bytes", name, dev->phys_block_size);
	}
#elif defined (BLKSSZGET)
	/* if we can't get physical block size, just use logical block size instead */
	if (dev->phys_block_size == -1) {
		if (ioctl(dev_fd(dev), BLKSSZGET, &dev->phys_block_size) < 0) {
			log_sys_error("ioctl BLKSSZGET", name);
			r = 0;
			goto out;
		}
		log_debug_devs("%s: Physical block size can't be determined: Using logical block size of %u bytes", name, dev->phys_block_size);
	}
#else
	/* if even BLKSSZGET is not available, use default 512b */
	if (dev->phys_block_size == -1) {
		dev->phys_block_size = 512;
		log_debug_devs("%s: Physical block size can't be determined: Using block size of %u bytes instead", name, dev->phys_block_size);
	}
#endif

	*physical_block_size = (unsigned int) dev->phys_block_size;
	*block_size = (unsigned int) dev->block_size;
out:
	if (needs_open && !dev_close(dev))
		stack;

	return r;
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

static int _aligned_io(struct device_area *where, char *write_buffer,
		       int should_write, dev_io_reason_t reason,
		       unsigned ioflags, lvm_callback_fn_t dev_read_callback_fn, void *dev_read_callback_context)
{
	unsigned int physical_block_size = 0;
	unsigned int block_size = 0;
	unsigned buffer_was_widened = 0;
	uintptr_t mask;
	struct device_area widened;
	struct device_buffer *devbuf;
	int r = 0;

	if (!(where->dev->flags & DEV_REGULAR) &&
	    !dev_get_block_size(where->dev, &physical_block_size, &block_size))
		return_0;

	if (!block_size)
		block_size = lvm_getpagesize();

	/* Apply minimum read size */
	if (!should_write && block_size < MIN_READ_SIZE)
		block_size = MIN_READ_SIZE;

	mask = block_size - 1;

	_widen_region(block_size, where, &widened);

	/* Did we widen the buffer?  When writing, this means means read-modify-write. */
	if (where->size != widened.size || where->start != widened.start) {
		buffer_was_widened = 1;
		log_debug_io("Widening request for %" PRIu64 " bytes at %" PRIu64 " to %" PRIu64 " bytes at %" PRIu64 " on %s (for %s)",
			     where->size, (uint64_t) where->start, widened.size, (uint64_t) widened.start, dev_name(where->dev), _reason_text(reason));
	} 

	devbuf = DEV_DEVBUF(where->dev, reason);
	_release_devbuf(devbuf);
	devbuf->where.dev = where->dev;
	devbuf->where.start = widened.start;
	devbuf->where.size = widened.size;
	devbuf->write = should_write;
	devbuf->reason = reason;
	devbuf->dev_read_callback_fn = dev_read_callback_fn;
	devbuf->dev_read_callback_context = dev_read_callback_context;

	/* Store location of requested data relative to start of buf */
	devbuf->data_offset = where->start - devbuf->where.start;

	if (should_write && !buffer_was_widened && !((uintptr_t) write_buffer & mask))
		/* Perform the I/O directly. */
		devbuf->buf = write_buffer;
	else if (!should_write)
		/* Postpone buffer allocation until we're about to issue the I/O */
		devbuf->buf = NULL;
	else {
		/* Allocate a bounce buffer with an extra block */
		if (!(devbuf->malloc_address = devbuf->buf = dm_malloc((size_t) devbuf->where.size + block_size))) {
			log_error("Bounce buffer malloc failed");
			return 0;
		}

		/*
		 * Realign start of bounce buffer (using the extra sector)
		 */
		if (((uintptr_t) devbuf->buf) & mask)
			devbuf->buf = (char *) ((((uintptr_t) devbuf->buf) + mask) & ~mask);
	}

	/* If we've reached our concurrent AIO limit, add this request to the queue */
	if (!devbuf->write && _aio_ctx && aio_supported_code_path(ioflags) && dev_read_callback_fn && _aio_must_queue) {
		_queue_aio(devbuf);
		return 1;
	}

	devbuf->write = 0;

	/* Do we need to read into the bounce buffer? */
	if ((!should_write || buffer_was_widened) && !_io(devbuf, ioflags)) {
		if (!should_write)
			goto_bad;
		/* FIXME Handle errors properly! */
		/* FIXME pre-extend the file */
		memset(devbuf->buf, '\n', devbuf->where.size);
	}

	if (!should_write)
		return 1;

	/* writes */

	if (devbuf->malloc_address) {
		memcpy((char *) devbuf->buf + devbuf->data_offset, write_buffer, (size_t) where->size);
		log_debug_io("Overwriting %" PRIu64 " bytes at %" PRIu64 " (for %s)", where->size,
			     (uint64_t) where->start, _reason_text(devbuf->reason));
	}

	/* ... then we write */
	devbuf->write = 1;
	if (!(r = _io(devbuf, 0)))
		goto_bad;

	_release_devbuf(devbuf);
	return 1;

bad:
	_release_devbuf(devbuf);
	return 0;
}

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

	if (dev->size_seqno == _dev_size_seqno) {
		log_very_verbose("%s: using cached size %" PRIu64 " sectors",
				 name, dev->size);
		*size = dev->size;
		return 1;
	}

	if (!dev_open_readonly(dev))
		return_0;

	if (ioctl(dev_fd(dev), BLKGETSIZE64, size) < 0) {
		log_sys_error("ioctl BLKGETSIZE64", name);
		if (!dev_close(dev))
			log_sys_error("close", name);
		return 0;
	}

	*size >>= BLKSIZE_SHIFT;	/* Convert to sectors */
	dev->size = *size;
	dev->size_seqno = _dev_size_seqno;

	if (!dev_close(dev))
		log_sys_error("close", name);

	log_very_verbose("%s: size is %" PRIu64 " sectors", name, *size);

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
		if (!dev_close(dev))
			stack;
		return 0;
	}

	*read_ahead = (uint32_t) read_ahead_long;
	dev->read_ahead = read_ahead_long;

	log_very_verbose("%s: read_ahead is %u sectors",
			 dev_name(dev), *read_ahead);

	if (!dev_close(dev))
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

	log_debug_devs("Discarding %" PRIu64 " bytes offset %" PRIu64 " bytes on %s.",
		       size_bytes, offset_bytes, dev_name(dev));
	if (ioctl(dev->fd, BLKDISCARD, &discard_range) < 0) {
		log_error("%s: BLKDISCARD ioctl at offset %" PRIu64 " size %" PRIu64 " failed: %s.",
			  dev_name(dev), offset_bytes, size_bytes, strerror(errno));
		if (!dev_close(dev))
			stack;
		/* It doesn't matter if discard failed, so return success. */
		return 1;
	}

	if (!dev_close(dev))
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

		dev_close_immediate(dev);
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
	dev->flags &= ~DEV_ACCESSED_W;

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
		dev_close_immediate(dev);
		return 0;
	}

#ifndef O_DIRECT_SUPPORT
	if (!(dev->flags & DEV_REGULAR))
		dev_flush(dev);
#endif

	if ((flags & O_CREAT) && !(flags & O_TRUNC))
		dev->end = lseek(dev->fd, (off_t) 0, SEEK_END);

	dm_list_add(&_open_devices, &dev->open_list);

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

int dev_test_excl(struct device *dev)
{
	int flags;
	int r;

	flags = vg_write_lock_held() ? O_RDWR : O_RDONLY;
	flags |= O_EXCL;

	r = dev_open_flags(dev, flags, 1, 1);
	if (r)
		dev_close_immediate(dev);

	return r;
}

static void _close(struct device *dev)
{
	if (close(dev->fd))
		log_sys_error("close", dev_name(dev));
	dev->fd = -1;
	dev->phys_block_size = -1;
	dev->block_size = -1;
	dm_list_del(&dev->open_list);
	devbufs_release(dev);

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

#ifndef O_DIRECT_SUPPORT
	if (dev->flags & DEV_ACCESSED_W)
		dev_flush(dev);
#endif

	if (dev->open_count > 0)
		dev->open_count--;

	if (immediate && dev->open_count)
		log_debug_devs("%s: Immediate close attempt while still referenced",
			       dev_name(dev));

	/* Close unless device is known to belong to a locked VG */
	if (immediate ||
	    (dev->open_count < 1 && !lvmcache_pvid_is_locked(dev->pvid)))
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

void dev_close_all(void)
{
	struct dm_list *doh, *doht;
	struct device *dev;

	dm_list_iterate_safe(doh, doht, &_open_devices) {
		dev = dm_list_struct_base(doh, struct device, open_list);
		if (dev->open_count < 1)
			_close(dev);
	}
}

static inline int _dev_is_valid(struct device *dev)
{
	return (dev->max_error_count == NO_DEV_ERROR_COUNT_LIMIT ||
		dev->error_count < dev->max_error_count);
}

static void _dev_inc_error_count(struct device *dev)
{
	if (++dev->error_count == dev->max_error_count)
		log_warn("WARNING: Error counts reached a limit of %d. "
			 "Device %s was disabled",
			 dev->max_error_count, dev_name(dev));
}

/*
 * Data is returned (read-only) at DEV_DEVBUF_DATA(dev, reason)
 */
int dev_read_callback(struct device *dev, uint64_t offset, size_t len, dev_io_reason_t reason,
		      unsigned ioflags, lvm_callback_fn_t dev_read_callback_fn, void *callback_context)
{
	struct device_area where;
	struct device_buffer *devbuf;
	uint64_t buf_end;
	int cached = 0;
	int ret = 1;

	if (!dev->open_count) {
		log_error(INTERNAL_ERROR "Attempt to access device %s while closed.", dev_name(dev));
		return 0;
	}

	if (!_dev_is_valid(dev))
		return 0;

	/*
	 * Can we satisfy this from data we stored last time we read?
	 */
	if ((devbuf = DEV_DEVBUF(dev, reason)) && devbuf->malloc_address) {
		buf_end = devbuf->where.start + devbuf->where.size - 1;
		if (offset >= devbuf->where.start && offset <= buf_end && offset + len - 1 <= buf_end) {
			/* Reuse this buffer */
			cached = 1;
			devbuf->data_offset = offset - devbuf->where.start;
			log_debug_io("Cached read for %" PRIu64 " bytes at %" PRIu64 " on %s (for %s)",
				     (uint64_t) len, (uint64_t) offset, dev_name(dev), _reason_text(reason));
			goto out;
		}
	}

	where.dev = dev;
	where.start = offset;
	where.size = len;

	ret = _aligned_io(&where, NULL, 0, reason, ioflags, dev_read_callback_fn, callback_context);
	if (!ret) {
		log_error("Read from %s failed", dev_name(dev));
		_dev_inc_error_count(dev);
	}

out:
	/* If we had an error or this was sync I/O, pass the result to any callback fn */
	if ((!ret || !_aio_ctx || !aio_supported_code_path(ioflags) || cached) && dev_read_callback_fn)
		dev_read_callback_fn(!ret, ioflags, callback_context, DEV_DEVBUF_DATA(dev, reason));

	return ret;
}

/* Returns pointer to read-only buffer. Caller does not free it.  */
const char *dev_read(struct device *dev, uint64_t offset, size_t len, dev_io_reason_t reason)
{
	if (!dev_read_callback(dev, offset, len, reason, 0, NULL, NULL))
		return_NULL;

	return DEV_DEVBUF_DATA(dev, reason);
}

/* Read into supplied retbuf owned by the caller. */
int dev_read_buf(struct device *dev, uint64_t offset, size_t len, dev_io_reason_t reason, void *retbuf)
{
	if (!dev_read_callback(dev, offset, len, reason, 0, NULL, NULL)) {
		log_error("Read from %s failed", dev_name(dev));
		return 0;
	}
	
	memcpy(retbuf, DEV_DEVBUF_DATA(dev, reason), len);

	return 1;
}

/*
 * Read from 'dev' in 2 distinct regions, denoted by (offset,len) and (offset2,len2).
 * Caller is responsible for dm_free().
 */
const char *dev_read_circular(struct device *dev, uint64_t offset, size_t len,
			uint64_t offset2, size_t len2, dev_io_reason_t reason)
{
	char *buf = NULL;

	if (!(buf = dm_malloc(len + len2))) {
		log_error("Buffer allocation failed for split metadata.");
		return NULL;
	}

	if (!dev_read_buf(dev, offset, len, reason, buf)) {
		log_error("Read from %s failed", dev_name(dev));
		dm_free(buf);
		return NULL;
	}

	if (!dev_read_buf(dev, offset2, len2, reason, buf + len)) {
		log_error("Circular read from %s failed", dev_name(dev));
		dm_free(buf);
		return NULL;
	}

	return buf;
}

/* FIXME If O_DIRECT can't extend file, dev_extend first; dev_truncate after.
 *       But fails if concurrent processes writing
 */

/* FIXME pre-extend the file */
int dev_append(struct device *dev, size_t len, dev_io_reason_t reason, char *buffer)
{
	int r;

	if (!dev->open_count)
		return_0;

	r = dev_write(dev, dev->end, len, reason, buffer);
	dev->end += (uint64_t) len;

#ifndef O_DIRECT_SUPPORT
	dev_flush(dev);
#endif
	return r;
}

int dev_write(struct device *dev, uint64_t offset, size_t len, dev_io_reason_t reason, void *buffer)
{
	struct device_area where;
	int ret;

	if (!dev->open_count)
		return_0;

	if (!_dev_is_valid(dev))
		return 0;

	if (!len) {
		log_error(INTERNAL_ERROR "Attempted to write 0 bytes to %s at " FMTu64, dev_name(dev), offset);
		return 0;
	}

	where.dev = dev;
	where.start = offset;
	where.size = len;

	dev->flags |= DEV_ACCESSED_W;

	ret = _aligned_io(&where, buffer, 1, reason, 0, NULL, NULL);
	if (!ret)
		_dev_inc_error_count(dev);

	return ret;
}

int dev_set(struct device *dev, uint64_t offset, size_t len, dev_io_reason_t reason, int value)
{
	size_t s;
	char buffer[4096] __attribute__((aligned(4096)));

	if (!dev_open(dev))
		return_0;

	if ((offset % SECTOR_SIZE) || (len % SECTOR_SIZE))
		log_debug_devs("Wiping %s at %" PRIu64 " length %" PRIsize_t,
			       dev_name(dev), offset, len);
	else
		log_debug_devs("Wiping %s at sector %" PRIu64 " length %" PRIsize_t
			       " sectors", dev_name(dev), offset >> SECTOR_SHIFT,
			       len >> SECTOR_SHIFT);

	memset(buffer, value, sizeof(buffer));
	while (1) {
		s = len > sizeof(buffer) ? sizeof(buffer) : len;
		if (!dev_write(dev, offset, s, reason, buffer))
			break;

		len -= s;
		if (!len)
			break;

		offset += s;
	}

	dev->flags |= DEV_ACCESSED_W;

	if (!dev_close(dev))
		stack;

	return (len == 0);
}
