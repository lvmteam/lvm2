/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
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

#include "lib/device/bcache.h"

#include "base/data-struct/radix-tree.h"
#include "lib/log/lvm-logging.h"
#include "lib/log/log.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <libaio.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/user.h>

#define SECTOR_SHIFT 9L

#define FD_TABLE_INC 1024
static int _fd_table_size = 0;
static int *_fd_table = NULL;


//----------------------------------------------------------------

static void log_sys_warn(const char *call)
{
	log_warn("WARNING: %s failed: %s.", call, strerror(errno));
}

// Assumes the list is not empty.
static inline struct dm_list *_list_pop(struct dm_list *head)
{
	struct dm_list *l;

	l = head->n;
	dm_list_del(l);
	return l;
}

//----------------------------------------------------------------

struct control_block {
	struct dm_list list;
	void *context;
	struct iocb cb;
};

struct cb_set {
	struct dm_list free;
	struct dm_list allocated;
	struct control_block vec[];
};

static struct cb_set *_cb_set_create(unsigned nr)
{
	unsigned i;
	struct cb_set *cbs = malloc(sizeof(*cbs) + nr * sizeof(*cbs->vec));

	if (!cbs)
		return NULL;

	dm_list_init(&cbs->free);
	dm_list_init(&cbs->allocated);

	for (i = 0; i < nr; i++)
		dm_list_add(&cbs->free, &cbs->vec[i].list);

	return cbs;
}

static void _cb_set_destroy(struct cb_set *cbs)
{
	// We know this is always called after a wait_all.  So there should
	// never be in flight IO.
	if (!dm_list_empty(&cbs->allocated)) {
		// bail out
		log_warn("WARNING: Async io still in flight.");
		return;
	}

	free(cbs);
}

static struct control_block *_cb_alloc(struct cb_set *cbs, void *context)
{
	struct control_block *cb;

	if (dm_list_empty(&cbs->free))
		return NULL;

	cb = dm_list_item(_list_pop(&cbs->free), struct control_block);
	cb->context = context;
	dm_list_add(&cbs->allocated, &cb->list);

	return cb;
}

static void _cb_free(struct cb_set *cbs, struct control_block *cb)
{
	dm_list_del(&cb->list);
	dm_list_add_h(&cbs->free, &cb->list);
}

static struct control_block *_iocb_to_cb(struct iocb *icb)
{
	return dm_list_struct_base(icb, struct control_block, cb);
}

//----------------------------------------------------------------

struct async_engine {
	struct io_engine e;
	io_context_t aio_context;
	struct cb_set *cbs;
	unsigned page_mask;
	pid_t aio_context_pid; /* PID that created this AIO context */
};

static struct async_engine *_to_async(struct io_engine *e)
{
	return container_of(e, struct async_engine, e);
}

static void _async_destroy(struct io_engine *ioe)
{
	struct async_engine *e = _to_async(ioe);

	_cb_set_destroy(e->cbs);

	/*
	 * Only call io_destroy() if we're in the same process that created
	 * the AIO context. After fork(), the child inherits the parent's
	 * aio_context value but must not call io_destroy() on it.
	 */
	if (e->aio_context) {
		if (e->aio_context_pid != getpid())
			log_debug("Skipping io_destroy() for different pid.");
		else if (io_destroy(e->aio_context)) // really slow
			log_sys_warn("io_destroy");
	}

	free(e);
}

static int _last_byte_di;
static uint64_t _last_byte_offset;
static int _last_byte_sector_size;

static bool _async_issue(struct io_engine *ioe, enum dir d, int di,
			 sector_t sb, sector_t se, void *data, void *context)
{
	int r;
	struct iocb *cb_array[1];
	struct control_block *cb;
	struct async_engine *e = _to_async(ioe);
	sector_t offset;
	sector_t nbytes;
	sector_t limit_nbytes;
	sector_t orig_nbytes;
	sector_t extra_nbytes = 0;

	if (((uintptr_t) data) & e->page_mask) {
		log_warn("misaligned data buffer");
		return false;
	}

	offset = sb << SECTOR_SHIFT;
	nbytes = (se - sb) << SECTOR_SHIFT;

	/*
	 * If bcache block goes past where lvm wants to write, then clamp it.
	 */
	if ((d == DIR_WRITE) && _last_byte_offset && (di == _last_byte_di)) {
		if (offset > _last_byte_offset) {
			log_error("Limit write at %llu len %llu beyond last byte %llu",
				  (unsigned long long)offset,
				  (unsigned long long)nbytes,
				  (unsigned long long)_last_byte_offset);
			return false;
		}

		/*
		 * If the bcache block offset+len goes beyond where lvm is
		 * intending to write, then reduce the len being written
		 * (which is the bcache block size) so we don't write past
		 * the limit set by lvm.  If after applying the limit, the
		 * resulting size is not a multiple of the sector size (512
		 * or 4096) then extend the reduced size to be a multiple of
		 * the sector size (we don't want to write partial sectors.)
		 */
		if (offset + nbytes > _last_byte_offset) {
			limit_nbytes = _last_byte_offset - offset;

			if (limit_nbytes % _last_byte_sector_size) {
				extra_nbytes = _last_byte_sector_size - (limit_nbytes % _last_byte_sector_size);

				/*
				 * adding extra_nbytes to the reduced nbytes (limit_nbytes)
				 * should make the final write size a multiple of the
				 * sector size.  This should never result in a final size
				 * larger than the bcache block size (as long as the bcache
				 * block size is a multiple of the sector size).
				 */
				if (limit_nbytes + extra_nbytes > nbytes) {
					log_warn("Skip extending write at %llu len %llu limit %llu extra %llu sector_size %llu",
						 (unsigned long long)offset,
						 (unsigned long long)nbytes,
						 (unsigned long long)limit_nbytes,
						 (unsigned long long)extra_nbytes,
						 (unsigned long long)_last_byte_sector_size);
					extra_nbytes = 0;
				}
			}

			orig_nbytes = nbytes;

			if (extra_nbytes) {
				log_debug("Limit write at %llu len %llu to len %llu rounded to %llu",
					  (unsigned long long)offset,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes,
					  (unsigned long long)(limit_nbytes + extra_nbytes));
				nbytes = limit_nbytes + extra_nbytes;
			} else {
				log_debug("Limit write at %llu len %llu to len %llu",
					  (unsigned long long)offset,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes);
				nbytes = limit_nbytes;
			}

			/*
			 * This shouldn't happen, the reduced+extended
			 * nbytes value should never be larger than the
			 * bcache block size.
			 */
			if (nbytes > orig_nbytes) {
				log_error("Invalid adjusted write at %llu len %llu adjusted %llu limit %llu extra %llu sector_size %llu",
					  (unsigned long long)offset,
					  (unsigned long long)orig_nbytes,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes,
					  (unsigned long long)extra_nbytes,
					  (unsigned long long)_last_byte_sector_size);
				return false;
			}
		}
	}

	cb = _cb_alloc(e->cbs, context);
	if (!cb) {
		log_warn("couldn't allocate control block");
		return false;
	}

	memset(&cb->cb, 0, sizeof(cb->cb));

	cb->cb.aio_fildes = (int) _fd_table[di];
	cb->cb.u.c.buf = data;
	cb->cb.u.c.offset = offset;
	cb->cb.u.c.nbytes = nbytes;
	cb->cb.aio_lio_opcode = (d == DIR_READ) ? IO_CMD_PREAD : IO_CMD_PWRITE;

#if 0
	if (d == DIR_READ) {
		log_debug("io R off %llu bytes %llu di %d fd %d",
			  (unsigned long long)cb->cb.u.c.offset,
			  (unsigned long long)cb->cb.u.c.nbytes,
			  di, _fd_table[di]);
	} else {
		log_debug("io W off %llu bytes %llu di %d fd %d",
			  (unsigned long long)cb->cb.u.c.offset,
			  (unsigned long long)cb->cb.u.c.nbytes,
			  di, _fd_table[di]);
	}
#endif

	cb_array[0] = &cb->cb;
	do {
		r = io_submit(e->aio_context, 1, cb_array);
	} while (r == -EAGAIN);

	if (r < 0) {
		_cb_free(e->cbs, cb);
		return false;
	}

	return true;
}

/*
 * MAX_IO is returned to the layer above via bcache_max_prefetches() which
 * tells the caller how many devices to submit io for concurrently.  There will
 * be an open file descriptor for each of these, so keep it low enough to avoid
 * reaching the default max open file limit (1024) when there are over 1024
 * devices being scanned.
 */

#define MAX_IO 256
#define MAX_EVENT 64

static bool _async_wait(struct io_engine *ioe, io_complete_fn fn)
{
	int i, r;
	struct io_event event[MAX_EVENT];
	struct control_block *cb;
	struct async_engine *e = _to_async(ioe);

	memset(&event, 0, sizeof(event));
	r = io_getevents(e->aio_context, 1, MAX_EVENT, event, NULL);

	if (r < 0) {
		log_sys_warn("io_getevents");
		return false;
	}

	for (i = 0; i < r; i++) {
		struct io_event *ev = event + i;

		cb = _iocb_to_cb((struct iocb *) ev->obj);

		if (ev->res == cb->cb.u.c.nbytes)
			fn((void *) cb->context, 0);

		else if ((int) ev->res < 0)
			fn(cb->context, (int) ev->res);

		// FIXME: dct added this. a short read is ok?!
		else if (ev->res >= (1 << SECTOR_SHIFT)) {
			/* minimum acceptable read is 1 sector */
			fn((void *) cb->context, 0);

		} else {
			fn(cb->context, -ENODATA);
		}

		_cb_free(e->cbs, cb);
	}

	return true;
}

static unsigned _async_max_io(struct io_engine *e)
{
	return MAX_IO;
}

struct io_engine *create_async_io_engine(void)
{
	static int _pagesize = 0;
	int r;
	struct async_engine *e;

	if ((_pagesize <= 0) && (_pagesize = sysconf(_SC_PAGESIZE)) < 0) {
		log_warn("_SC_PAGESIZE returns negative value.");
		return NULL;
	}

	if (!(e = malloc(sizeof(*e))))
		return NULL;

	e->e.destroy = _async_destroy;
	e->e.issue = _async_issue;
	e->e.wait = _async_wait;
	e->e.max_io = _async_max_io;

	e->aio_context = 0;
	e->aio_context_pid = getpid();
	r = io_setup(MAX_IO, &e->aio_context);
	if (r < 0) {
		log_debug("io_setup failed %d", r);
		free(e);
		return NULL;
	}

	e->cbs = _cb_set_create(MAX_IO);
	if (!e->cbs) {
		log_warn("couldn't create control block set");
		free(e);
		return NULL;
	}

	e->page_mask = (unsigned) _pagesize - 1;

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->e;
}

//----------------------------------------------------------------

struct sync_io {
        struct dm_list list;
	void *context;
};

struct sync_engine {
	struct io_engine e;
	struct dm_list complete;
};

static struct sync_engine *_to_sync(struct io_engine *e)
{
        return container_of(e, struct sync_engine, e);
}

static void _sync_destroy(struct io_engine *ioe)
{
        struct sync_engine *e = _to_sync(ioe);
        free(e);
}

static bool _sync_issue(struct io_engine *ioe, enum dir d, int di,
                        sector_t sb, sector_t se, void *data, void *context)
{
	int rv;
	off_t off;
	uint64_t where;
	uint64_t pos = 0;
	uint64_t len = (se - sb) * 512;
	struct sync_engine *e = _to_sync(ioe);
	struct sync_io *io = malloc(sizeof(*io));
	if (!io) {
		log_warn("unable to allocate sync_io");
        	return false;
	}

	where = sb * 512;
	off = lseek(_fd_table[di], where, SEEK_SET);
	if (off == (off_t) -1) {
		log_warn("Device seek error %d for offset %llu", errno, (unsigned long long)where);
		free(io);
		return false;
	}
	if (off != (off_t) where) {
		log_warn("Device seek failed for offset %llu", (unsigned long long)where);
		free(io);
		return false;
	}

	/*
	 * If bcache block goes past where lvm wants to write, then clamp it.
	 */
	if ((d == DIR_WRITE) && _last_byte_offset && (di == _last_byte_di)) {
		uint64_t offset = where;
		uint64_t nbytes = len;
		sector_t limit_nbytes = 0;
		sector_t extra_nbytes = 0;
		sector_t orig_nbytes = 0;

		if (offset > _last_byte_offset) {
			log_error("Limit write at %llu len %llu beyond last byte %llu",
				  (unsigned long long)offset,
				  (unsigned long long)nbytes,
				  (unsigned long long)_last_byte_offset);
			free(io);
			return false;
		}

		if (offset + nbytes > _last_byte_offset) {
			limit_nbytes = _last_byte_offset - offset;

			if (limit_nbytes % _last_byte_sector_size) {
				extra_nbytes = _last_byte_sector_size - (limit_nbytes % _last_byte_sector_size);

				/*
				 * adding extra_nbytes to the reduced nbytes (limit_nbytes)
				 * should make the final write size a multiple of the
				 * sector size.  This should never result in a final size
				 * larger than the bcache block size (as long as the bcache
				 * block size is a multiple of the sector size).
				 */
				if (limit_nbytes + extra_nbytes > nbytes) {
					log_warn("Skip extending write at %llu len %llu limit %llu extra %llu sector_size %llu",
						 (unsigned long long)offset,
						 (unsigned long long)nbytes,
						 (unsigned long long)limit_nbytes,
						 (unsigned long long)extra_nbytes,
						 (unsigned long long)_last_byte_sector_size);
					extra_nbytes = 0;
				}
			}

			orig_nbytes = nbytes;

			if (extra_nbytes) {
				log_debug("Limit write at %llu len %llu to len %llu rounded to %llu",
					  (unsigned long long)offset,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes,
					  (unsigned long long)(limit_nbytes + extra_nbytes));
				nbytes = limit_nbytes + extra_nbytes;
			} else {
				log_debug("Limit write at %llu len %llu to len %llu",
					  (unsigned long long)offset,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes);
				nbytes = limit_nbytes;
			}

			/*
			 * This shouldn't happen, the reduced+extended
			 * nbytes value should never be larger than the
			 * bcache block size.
			 */
			if (nbytes > orig_nbytes) {
				log_error("Invalid adjusted write at %llu len %llu adjusted %llu limit %llu extra %llu sector_size %llu",
					  (unsigned long long)offset,
					  (unsigned long long)orig_nbytes,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes,
					  (unsigned long long)extra_nbytes,
					  (unsigned long long)_last_byte_sector_size);
				free(io);
				return false;
			}
		}

		where = offset;
		len = nbytes;
	}

	while (pos < len) {
		if (d == DIR_READ)
			rv = read(_fd_table[di], (char *)data + pos, len - pos);
		else
			rv = write(_fd_table[di], (char *)data + pos, len - pos);

		if (rv == -1 && errno == EINTR)
			continue;
		if (rv == -1 && errno == EAGAIN)
			continue;

		if (!rv)
			break;

		if (rv < 0) {
			if (d == DIR_READ)
				log_debug("Device read error %d offset %llu len %llu", errno,
					  (unsigned long long)(where + pos),
					  (unsigned long long)(len - pos));
			else
				log_debug("Device write error %d offset %llu len %llu", errno,
					  (unsigned long long)(where + pos),
					  (unsigned long long)(len - pos));
			free(io);
			return false;
		}
		pos += rv;
	}

	if (pos < len) {
		if (d == DIR_READ)
			log_warn("Device read short %u bytes remaining", (unsigned)(len - pos));
		else
			log_warn("Device write short %u bytes remaining", (unsigned)(len - pos));
		/*
        	free(io);
        	return false;
		*/
	}


	dm_list_add(&e->complete, &io->list);
	io->context = context;

	return true;
}

static bool _sync_wait(struct io_engine *ioe, io_complete_fn fn)
{
        struct sync_io *io, *tmp;
	struct sync_engine *e = _to_sync(ioe);

	dm_list_iterate_items_safe(io, tmp, &e->complete) {
		fn(io->context, 0);
		dm_list_del(&io->list);
		free(io);
	}

	return true;
}

static unsigned _sync_max_io(struct io_engine *e)
{
        return 1;
}

struct io_engine *create_sync_io_engine(void)
{
	struct sync_engine *e = malloc(sizeof(*e));

	if (!e)
        	return NULL;

        e->e.destroy = _sync_destroy;
        e->e.issue = _sync_issue;
        e->e.wait = _sync_wait;
        e->e.max_io = _sync_max_io;

	dm_list_init(&e->complete);
	/* coverity[leaked_storage] 'e' is not leaking */
        return &e->e;
}

//----------------------------------------------------------------

#define MIN_BLOCKS 16
#define WRITEBACK_LOW_THRESHOLD_PERCENT 33
#define WRITEBACK_HIGH_THRESHOLD_PERCENT 66

//----------------------------------------------------------------

static void *_alloc_aligned(size_t len, size_t alignment)
{
	void *result = NULL;
	int r = posix_memalign(&result, alignment, len);
	if (r)
		return NULL;

	return result;
}

//----------------------------------------------------------------

static bool _test_flags(struct block *b, unsigned bits)
{
	return (b->flags & bits) != 0;
}

static void _set_flags(struct block *b, unsigned bits)
{
	b->flags |= bits;
}

static void _clear_flags(struct block *b, unsigned bits)
{
	b->flags &= ~bits;
}

//----------------------------------------------------------------

enum block_flags {
	BF_IO_PENDING = (1 << 0),
	BF_DIRTY = (1 << 1),
};

struct bcache {
	sector_t block_sectors;
	uint64_t nr_data_blocks;
	uint64_t nr_cache_blocks;
	unsigned max_io;

	struct io_engine *engine;

	void *raw_data;
	struct block *raw_blocks;

	/*
	 * Lists that categorize the blocks.
	 */
	unsigned nr_locked;
	unsigned nr_dirty;
	unsigned nr_io_pending;

	struct dm_list free;
	struct dm_list errored;
	struct dm_list dirty;
	struct dm_list clean;
	struct dm_list io_pending;

	struct radix_tree *rtree;

	/*
	 * Statistics
	 */
	unsigned read_hits;
	unsigned read_misses;
	unsigned write_zeroes;
	unsigned write_hits;
	unsigned write_misses;
	unsigned prefetches;
};

//----------------------------------------------------------------

struct key_parts {
	uint32_t di;
	uint64_t b;
} __attribute__ ((packed));

union key {
	struct key_parts parts;
        uint8_t bytes[12];
};

static struct block *_block_lookup(struct bcache *cache, int di, uint64_t i)
{
	union key k;
	union radix_value v;

	k.parts.di = di;
	k.parts.b = i;

	if (radix_tree_lookup(cache->rtree, k.bytes, sizeof(k.bytes), &v))
		return v.ptr;

	return NULL;
}

static bool _block_insert(struct block *b)
{
        union key k;
        union radix_value v;

        k.parts.di = b->di;
        k.parts.b = b->index;
        v.ptr = b;

	return radix_tree_insert(b->cache->rtree, k.bytes, sizeof(k.bytes), v);
}

static void _block_remove(struct block *b)
{
        union key k;

        k.parts.di = b->di;
        k.parts.b = b->index;

	(void) radix_tree_remove(b->cache->rtree, k.bytes, sizeof(k.bytes));
}

//----------------------------------------------------------------

static bool _init_free_list(struct bcache *cache, unsigned count, unsigned pgsize)
{
	unsigned i;
	size_t block_size = cache->block_sectors << SECTOR_SHIFT;
	unsigned char *data =
		(unsigned char *) _alloc_aligned(count * block_size, pgsize);

	/* Allocate the data for each block.  We page align the data. */
	if (!data)
		return false;

	cache->raw_blocks = malloc(count * sizeof(*cache->raw_blocks));
	if (!cache->raw_blocks) {
		free(data);
		return false;
	}

	cache->raw_data = data;

	for (i = 0; i < count; i++) {
		struct block *b = cache->raw_blocks + i;
		b->cache = cache;
		b->data = data + (block_size * i);
		dm_list_add(&cache->free, &b->list);
	}

	return true;
}

static void _exit_free_list(struct bcache *cache)
{
	free(cache->raw_data);
	free(cache->raw_blocks);
}

static struct block *_alloc_block(struct bcache *cache)
{
	if (dm_list_empty(&cache->free))
		return NULL;

	return dm_list_struct_base(_list_pop(&cache->free), struct block, list);
}

static void _free_block(struct block *b)
{
	dm_list_add(&b->cache->free, &b->list);
}

/*----------------------------------------------------------------
 * Clean/dirty list management.
 * Always use these methods to ensure nr_dirty_ is correct.
 *--------------------------------------------------------------*/

static void _unlink_block(struct block *b)
{
	if (_test_flags(b, BF_DIRTY))
		b->cache->nr_dirty--;

	dm_list_del(&b->list);
}

static void _link_block(struct block *b)
{
	struct bcache *cache = b->cache;

	if (_test_flags(b, BF_DIRTY)) {
		dm_list_add(&cache->dirty, &b->list);
		cache->nr_dirty++;
	} else
		dm_list_add(&cache->clean, &b->list);
}

static void _relink(struct block *b)
{
	_unlink_block(b);
	_link_block(b);
}

/*----------------------------------------------------------------
 * Low level IO handling
 *
 * We cannot have two concurrent writes on the same block.
 * eg, background writeback, put with dirty, flush?
 *
 * To avoid this we introduce some restrictions:
 *
 * i)  A held block can never be written back.
 * ii) You cannot get a block until writeback has completed.
 *
 *--------------------------------------------------------------*/

static void _complete_io(void *context, int err)
{
	struct block *b = context;
	struct bcache *cache = b->cache;

	b->error = err;
	_clear_flags(b, BF_IO_PENDING);
	cache->nr_io_pending--;

	/*
	 * b is on the io_pending list, so we don't want to use unlink_block.
	 * Which would incorrectly adjust nr_dirty.
	 */
	dm_list_del(&b->list);

	if (b->error) {
		dm_list_add(&cache->errored, &b->list);

	} else {
		_clear_flags(b, BF_DIRTY);
		_link_block(b);
	}
}

/*
 * |b->list| should be valid (either pointing to itself, on one of the other
 * lists.
 */
static void _issue_low_level(struct block *b, enum dir d)
{
	struct bcache *cache = b->cache;
	sector_t sb = b->index * cache->block_sectors;
	sector_t se = sb + cache->block_sectors;

	if (_test_flags(b, BF_IO_PENDING))
		return;

	b->io_dir = d;
	_set_flags(b, BF_IO_PENDING);
	cache->nr_io_pending++;

	dm_list_move(&cache->io_pending, &b->list);

	if (!cache->engine->issue(cache->engine, d, b->di, sb, se, b->data, b)) {
		/* FIXME: if io_submit() set an errno, return that instead of EIO? */
		_complete_io(b, -EIO);
		return;
	}
}

static inline void _issue_read(struct block *b)
{
	_issue_low_level(b, DIR_READ);
}

static inline void _issue_write(struct block *b)
{
	_issue_low_level(b, DIR_WRITE);
}

static bool _wait_io(struct bcache *cache)
{
	return cache->engine->wait(cache->engine, _complete_io);
}

/*----------------------------------------------------------------
 * High level IO handling
 *--------------------------------------------------------------*/

static void _wait_all(struct bcache *cache)
{
	while (!dm_list_empty(&cache->io_pending))
		_wait_io(cache);
}

static void _wait_specific(struct block *b)
{
	while (_test_flags(b, BF_IO_PENDING))
		_wait_io(b->cache);
}

static unsigned _writeback(struct bcache *cache, unsigned count)
{
	unsigned actual = 0;
	struct block *b, *tmp;

	dm_list_iterate_items_gen_safe (b, tmp, &cache->dirty, list) {
		if (actual == count)
			break;

		// We can't writeback anything that's still in use.
		if (!b->ref_count) {
			_issue_write(b);
			actual++;
		}
	}

	return actual;
}

/*----------------------------------------------------------------
 * High level allocation
 *--------------------------------------------------------------*/

static struct block *_find_unused_clean_block(struct bcache *cache)
{
	struct block *b;

	dm_list_iterate_items (b, &cache->clean) {
		if (!b->ref_count) {
			_unlink_block(b);
			_block_remove(b);
			return b;
		}
	}

	return NULL;
}

static struct block *_new_block(struct bcache *cache, int di, block_address i, bool can_wait)
{
	struct block *b;

	b = _alloc_block(cache);
	while (!b) {
		b = _find_unused_clean_block(cache);
		if (!b) {
			if (can_wait) {
				if (dm_list_empty(&cache->io_pending))
					_writeback(cache, 16);  // FIXME: magic number
				_wait_all(cache);
				if (dm_list_size(&cache->errored) >= cache->max_io) {
					log_debug("bcache no new blocks for di %d index %u with >%d errors.",
						  di, (uint32_t) i, cache->max_io);
					return NULL;
				}
			} else {
				log_debug("bcache no new blocks for di %d index %u",
					  di, (uint32_t) i);
				return NULL;
			}
		}
	}

	if (b) {
		dm_list_init(&b->list);
		b->flags = 0;
		b->di = di;
		b->index = i;
		b->ref_count = 0;
		b->error = 0;

		if (!_block_insert(b)) {
        		log_error("bcache unable to insert block in radix tree (OOM?)");
			_free_block(b);
			return NULL;
		}
	}

	return b;
}

/*----------------------------------------------------------------
 * Block reference counting
 *--------------------------------------------------------------*/
static void _zero_block(struct block *b)
{
	b->cache->write_zeroes++;
	memset(b->data, 0, b->cache->block_sectors << SECTOR_SHIFT);
	_set_flags(b, BF_DIRTY);
}

static void _hit(struct block *b, unsigned flags)
{
	struct bcache *cache = b->cache;

	if (flags & (GF_ZERO | GF_DIRTY))
		cache->write_hits++;
	else
		cache->read_hits++;

	_relink(b);
}

static void _miss(struct bcache *cache, unsigned flags)
{
	if (flags & (GF_ZERO | GF_DIRTY))
		cache->write_misses++;
	else
		cache->read_misses++;
}

static struct block *_lookup_or_read_block(struct bcache *cache,
				  	   int di, block_address i,
					   unsigned flags)
{
	struct block *b = _block_lookup(cache, di, i);

	if (b) {
		// FIXME: this is insufficient.  We need to also catch a read
		// lock of a write locked block.  Ref count needs to distinguish.
		if (b->ref_count && (flags & (GF_DIRTY | GF_ZERO))) {
			log_warn("concurrent write lock attempted");
			return NULL;
		}

		if (_test_flags(b, BF_IO_PENDING)) {
			_miss(cache, flags);
			_wait_specific(b);

		} else
			_hit(b, flags);

		_unlink_block(b);

		if (flags & GF_ZERO)
			_zero_block(b);

	} else {
		_miss(cache, flags);

		b = _new_block(cache, di, i, true);
		if (b) {
			if (flags & GF_ZERO)
				_zero_block(b);

			else {
				_issue_read(b);
				_wait_specific(b);

				// we know the block is clean and unerrored.
				_unlink_block(b);
			}
		}
	}

	if (b) {
		if (flags & (GF_DIRTY | GF_ZERO))
			_set_flags(b, BF_DIRTY);

		_link_block(b);
		return b;
	}

	return NULL;
}

static void _preemptive_writeback(struct bcache *cache)
{
	// FIXME: this ignores those blocks that are in the error state.  Track
	// nr_clean instead?
	unsigned nr_available = cache->nr_cache_blocks - (cache->nr_dirty - cache->nr_io_pending);
	if (nr_available < (WRITEBACK_LOW_THRESHOLD_PERCENT * cache->nr_cache_blocks / 100))
		_writeback(cache, (WRITEBACK_HIGH_THRESHOLD_PERCENT * cache->nr_cache_blocks / 100) - nr_available);

}

/*----------------------------------------------------------------
 * Public interface
 *--------------------------------------------------------------*/
struct bcache *bcache_create(sector_t block_sectors, unsigned nr_cache_blocks,
			     struct io_engine *engine)
{
	static long _pagesize = 0;
	struct bcache *cache;
	unsigned max_io = engine->max_io(engine);
	int i;

	if ((_pagesize <= 0) && ((_pagesize = sysconf(_SC_PAGESIZE)) < 0)) {
		log_warn("WARNING: _SC_PAGESIZE returns negative value.");
		return NULL;
	}

	if (!nr_cache_blocks) {
		log_warn("bcache must have at least one cache block");
		return NULL;
	}

	if (!block_sectors) {
		log_warn("bcache must have a non zero block size");
		return NULL;
	}

	if (block_sectors & ((_pagesize >> SECTOR_SHIFT) - 1)) {
		log_warn("bcache block size must be a multiple of page size");
		return NULL;
	}

	cache = malloc(sizeof(*cache));
	if (!cache)
		return NULL;

	cache->block_sectors = block_sectors;
	cache->nr_cache_blocks = nr_cache_blocks;
	cache->max_io = nr_cache_blocks < max_io ? nr_cache_blocks : max_io;
	cache->engine = engine;
	cache->nr_locked = 0;
	cache->nr_dirty = 0;
	cache->nr_io_pending = 0;

	dm_list_init(&cache->free);
	dm_list_init(&cache->errored);
	dm_list_init(&cache->dirty);
	dm_list_init(&cache->clean);
	dm_list_init(&cache->io_pending);

        cache->rtree = radix_tree_create(NULL, NULL);
	if (!cache->rtree) {
		cache->engine->destroy(cache->engine);
		free(cache);
		return NULL;
	}

	cache->read_hits = 0;
	cache->read_misses = 0;
	cache->write_zeroes = 0;
	cache->write_hits = 0;
	cache->write_misses = 0;
	cache->prefetches = 0;

	if (!_init_free_list(cache, nr_cache_blocks, _pagesize)) {
		cache->engine->destroy(cache->engine);
		radix_tree_destroy(cache->rtree);
		free(cache);
		return NULL;
	}

	_fd_table_size = FD_TABLE_INC;

	if (!(_fd_table = malloc(sizeof(int) * _fd_table_size))) {
		cache->engine->destroy(cache->engine);
		radix_tree_destroy(cache->rtree);
		free(cache);
		return NULL;
	}

	for (i = 0; i < _fd_table_size; i++)
		_fd_table[i] = -1;

	return cache;
}

void bcache_destroy(struct bcache *cache)
{
	if (cache->nr_locked)
		log_warn("some blocks are still locked");

	if (!bcache_flush(cache))
		stack;
	_exit_free_list(cache);
	radix_tree_destroy(cache->rtree);
	cache->engine->destroy(cache->engine);
	free(cache);
	free(_fd_table);
	_fd_table = NULL;
	_fd_table_size = 0;
}

sector_t bcache_block_sectors(struct bcache *cache)
{
	return cache->block_sectors;
}

unsigned bcache_nr_cache_blocks(struct bcache *cache)
{
	return cache->nr_cache_blocks;
}

unsigned bcache_max_prefetches(struct bcache *cache)
{
	return cache->max_io;
}

void bcache_prefetch(struct bcache *cache, int di, block_address i)
{
	struct block *b = _block_lookup(cache, di, i);

	if (!b) {
		if (cache->nr_io_pending < cache->max_io) {
			b = _new_block(cache, di, i, false);
			if (b) {
				cache->prefetches++;
				_issue_read(b);
			}
		}
	}
}

//----------------------------------------------------------------

static void _recycle_block(struct bcache *cache, struct block *b)
{
	_unlink_block(b);
	_block_remove(b);
	_free_block(b);
}

bool bcache_get(struct bcache *cache, int di, block_address i,
	        unsigned flags, struct block **result)
{
	struct block *b;

	if (di >= _fd_table_size)
		goto bad;

	b = _lookup_or_read_block(cache, di, i, flags);
	if (b) {
		if (b->error) {
			if (b->io_dir == DIR_READ) {
				// Now we know the read failed we can just forget
				// about this block, since there's no dirty data to
				// be written back.
				_recycle_block(cache, b);
			}
			return false;
		}

		if (!b->ref_count)
			cache->nr_locked++;
		b->ref_count++;

		*result = b;
		return true;
	}
bad:
	*result = NULL;

	log_error("bcache failed to get block %u di %d", (uint32_t) i, di);
	return false;
}

//----------------------------------------------------------------

static void _put_ref(struct block *b)
{
	if (!b->ref_count) {
		log_warn("ref count on bcache block already zero");
		return;
	}

	b->ref_count--;
	if (!b->ref_count)
		b->cache->nr_locked--;
}

void bcache_put(struct block *b)
{
	_put_ref(b);

	if (_test_flags(b, BF_DIRTY))
		_preemptive_writeback(b->cache);
}

//----------------------------------------------------------------

bool bcache_flush(struct bcache *cache)
{
	// Only dirty data is on the errored list, since bad read blocks get
	// recycled straight away.  So we put these back on the dirty list, and
	// try and rewrite everything.
	dm_list_splice(&cache->dirty, &cache->errored);

	while (!dm_list_empty(&cache->dirty)) {
		struct block *b = dm_list_item(_list_pop(&cache->dirty), struct block);
		if (b->ref_count || _test_flags(b, BF_IO_PENDING)) {
			// The superblock may well be still locked.
			continue;
		}

		_issue_write(b);
	}

	_wait_all(cache);

	return dm_list_empty(&cache->errored);
}

//----------------------------------------------------------------
/*
 * You can safely call this with a NULL block.
 */
static bool _invalidate_block(struct bcache *cache, struct block *b)
{
	if (!b)
		return true;

	if (_test_flags(b, BF_IO_PENDING))
		_wait_specific(b);

	if (b->ref_count) {
		log_warn("bcache_invalidate: block (%d, %llu) still held",
			 b->di, (unsigned long long) b->index);
		return false;
	}

	if (_test_flags(b, BF_DIRTY)) {
		_issue_write(b);
		_wait_specific(b);

		if (b->error)
        		return false;
	}

	_recycle_block(cache, b);

	return true;
}

bool bcache_invalidate(struct bcache *cache, int di, block_address i)
{
	return _invalidate_block(cache, _block_lookup(cache, di, i));
}

//----------------------------------------------------------------

struct invalidate_iterator {
	bool success;
	struct radix_tree_iterator it;
};

static bool _writeback_v(struct radix_tree_iterator *it,
                         const void *kb, size_t keylen, union radix_value v)
{
	struct block *b = v.ptr;

	if (_test_flags(b, BF_DIRTY))
		_issue_write(b);

	return true;
}

static bool _invalidate_v(struct radix_tree_iterator *it,
                          const void *kb, size_t keylen, union radix_value v)
{
	struct block *b = v.ptr;
	struct invalidate_iterator *iit = container_of(it, struct invalidate_iterator, it);

	if (b->error || _test_flags(b, BF_DIRTY)) {
		log_warn("WARNING: bcache_invalidate: block (%d, %llu) still dirty.",
			 b->di, (unsigned long long) b->index);
		iit->success = false;
		return true;
	}

	if (b->ref_count) {
		log_warn("WARNING: bcache_invalidate: block (%d, %llu) still held.",
			 b->di, (unsigned long long) b->index);
		iit->success = false;
		return true;
	}

	_unlink_block(b);
	_free_block(b);

	// We can't remove the block from the radix tree yet because
	// we're in the middle of an iteration.
	return true;
}

bool bcache_invalidate_di(struct bcache *cache, int di)
{
	union key k;
	struct invalidate_iterator it;

	k.parts.di = di;

	it.it.visit = _writeback_v;
	radix_tree_iterate(cache->rtree, k.bytes, sizeof(k.parts.di), &it.it);

	_wait_all(cache);

	it.success = true;
	it.it.visit = _invalidate_v;
	radix_tree_iterate(cache->rtree, k.bytes, sizeof(k.parts.di), &it.it);

	if (it.success)
		(void) radix_tree_remove_prefix(cache->rtree, k.bytes, sizeof(k.parts.di));

	return it.success;
}

//----------------------------------------------------------------

static bool _abort_v(struct radix_tree_iterator *it,
                     const void *kb, size_t keylen, union radix_value v)
{
	struct block *b = v.ptr;

	if (b->ref_count) {
		log_fatal("bcache_abort: block (%d, %llu) still held",
			 b->di, (unsigned long long) b->index);
		return true;
	}

	_unlink_block(b);
	_free_block(b);

	// We can't remove the block from the radix tree yet because
	// we're in the middle of an iteration.
	return true;
}

void bcache_abort_di(struct bcache *cache, int di)
{
	union key k;
	struct radix_tree_iterator it;

	k.parts.di = di;

	it.visit = _abort_v;
	radix_tree_iterate(cache->rtree, k.bytes, sizeof(k.parts.di), &it);
	(void) radix_tree_remove_prefix(cache->rtree, k.bytes, sizeof(k.parts.di));
}

//----------------------------------------------------------------

void bcache_set_last_byte(struct bcache *cache, int di, uint64_t offset, int sector_size)
{
	_last_byte_di = di;
	_last_byte_offset = offset;
	_last_byte_sector_size = sector_size;
	if (!sector_size)
		_last_byte_sector_size = 512;
}

void bcache_unset_last_byte(struct bcache *cache, int di)
{
	if (_last_byte_di == di) {
		_last_byte_di = 0;
		_last_byte_offset = 0;
		_last_byte_sector_size = 0;
	}
}

int bcache_set_fd(int fd)
{
	int *new_table = NULL;
	int new_size = 0;
	int i;

 retry:
	for (i = 0; i < _fd_table_size; i++) {
		if (_fd_table[i] == -1) {
			_fd_table[i] = fd;
			return i;
		}
	}

	/* already tried once, shouldn't happen */
	if (new_size)
		return -1;

	new_size = _fd_table_size + FD_TABLE_INC;

	new_table = realloc(_fd_table, sizeof(int) * new_size);
	if (!new_table) {
		log_error("Cannot extend bcache fd table");
		return -1;
	}

	for (i = _fd_table_size; i < new_size; i++)
		new_table[i] = -1;

	_fd_table = new_table;
	_fd_table_size = new_size;

	goto retry;
}

/*
 * Should we check for unflushed or in-progress io on an fd
 * prior to doing clear_fd or change_fd?  (To catch mistakes;
 * the caller should be smart enough to not do that.)
 */

void bcache_clear_fd(int di)
{
	if (di >= _fd_table_size)
		return;
	_fd_table[di] = -1;
}

int bcache_change_fd(int di, int fd)
{
	if (di >= _fd_table_size)
		return 0;
	if (di < 0) {
		log_error(INTERNAL_ERROR "Cannot change not opened DI with FD:%d", fd);
		return 0;
	}
	_fd_table[di] = fd;
	return 1;
}
