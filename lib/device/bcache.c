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

#define _GNU_SOURCE

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
#include <sys/ioctl.h>
#include <sys/user.h>

#include "bcache.h"
#include "dm-logging.h"
#include "log.h"

#define SECTOR_SHIFT 9L

//----------------------------------------------------------------

static void log_sys_warn(const char *syscall)
{
	log_warn("%s failed: %s", syscall, strerror(errno));
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
	struct control_block *vec;
} control_block_set;

static struct cb_set *_cb_set_create(unsigned nr)
{
	int i;
	struct cb_set *cbs = dm_malloc(sizeof(*cbs));

	if (!cbs)
		return NULL;

	cbs->vec = dm_malloc(nr * sizeof(*cbs->vec));
	if (!cbs->vec) {
		dm_free(cbs);
		return NULL;
	}

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
		log_error("async io still in flight");
		return;
	}

	dm_free(cbs->vec);
	dm_free(cbs);
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

// FIXME: write a sync engine too
enum dir {
	DIR_READ,
	DIR_WRITE
};

struct io_engine {
	io_context_t aio_context;
	struct cb_set *cbs;
};

static struct io_engine *_engine_create(unsigned max_io)
{
	int r;
	struct io_engine *e = dm_malloc(sizeof(*e));

	if (!e)
		return NULL;

	e->aio_context = 0;
	r = io_setup(max_io, &e->aio_context);
	if (r < 0) {
		log_warn("io_setup failed");
		return NULL;
	}

	e->cbs = _cb_set_create(max_io);
	if (!e->cbs) {
		log_warn("couldn't create control block set");
		dm_free(e);
		return NULL;
	}

	return e;
}

static void _engine_destroy(struct io_engine *e)
{
	int r;

	_cb_set_destroy(e->cbs);

	// io_destroy is really slow
	r = io_destroy(e->aio_context);
	if (r)
		log_sys_warn("io_destroy");

	dm_free(e);
}

static bool _engine_issue(struct io_engine *e, enum dir d, int fd,
			  sector_t sb, sector_t se, void *data, void *context)
{
	int r;
	struct iocb *cb_array[1];
	struct control_block *cb;

	if (((uint64_t) data) & (PAGE_SIZE - 1)) {
		log_warn("misaligned data buffer");
		return false;
	}

	cb = _cb_alloc(e->cbs, context);
	if (!cb) {
		log_warn("couldn't allocate control block");
		return false;
	}

	memset(&cb->cb, 0, sizeof(cb->cb));

	cb->cb.aio_fildes = (int) fd;
	cb->cb.u.c.buf = data;
	cb->cb.u.c.offset = sb << SECTOR_SHIFT;
	cb->cb.u.c.nbytes = (se - sb) << SECTOR_SHIFT;
	cb->cb.aio_lio_opcode = (d == DIR_READ) ? IO_CMD_PREAD : IO_CMD_PWRITE;

	cb_array[0] = &cb->cb;
	r = io_submit(e->aio_context, 1, cb_array);
	if (r < 0) {
		log_sys_warn("io_submit");
		_cb_free(e->cbs, cb);
		return false;
	}

	return true;
}

#define MAX_IO 1024
#define MAX_EVENT 64
typedef void complete_fn(void *context, int io_error);

static bool _engine_wait(struct io_engine *e, complete_fn fn)
{
	int i, r;
	struct io_event event[MAX_EVENT];
	struct control_block *cb;

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

		else {
			log_warn("short io");
			fn(cb->context, -ENODATA);
		}

		_cb_free(e->cbs, cb);
	}

	return true;
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
	 * Lists that categorise the blocks.
	 */
	unsigned nr_locked;
	unsigned nr_dirty;
	unsigned nr_io_pending;

	struct dm_list free;
	struct dm_list errored;
	struct dm_list dirty;
	struct dm_list clean;
	struct dm_list io_pending;

	/*
	 * Hash table.
	 */
	unsigned nr_buckets;
	unsigned hash_mask;
	struct dm_list *buckets;

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

/*  2^63 + 2^61 - 2^57 + 2^54 - 2^51 - 2^18 + 1 */
#define GOLDEN_RATIO_PRIME_64 0x9e37fffffffc0001UL

static unsigned _hash(struct bcache *cache, int fd, uint64_t index)
{
	uint64_t h = (index << 10) & fd;
	h *= GOLDEN_RATIO_PRIME_64;
	return h & cache->hash_mask;
}

static struct block *_hash_lookup(struct bcache *cache, int fd, uint64_t index)
{
	struct block *b;
	unsigned h = _hash(cache, fd, index);

	dm_list_iterate_items_gen (b, cache->buckets + h, hash)
		if (b->index == index)
			return b;

	return NULL;
}

static void _hash_insert(struct block *b)
{
	unsigned h = _hash(b->cache, b->fd, b->index);
	dm_list_add_h(b->cache->buckets + h, &b->hash);
}

static inline void _hash_remove(struct block *b)
{
	dm_list_del(&b->hash);
}

/*
 * Must return a power of 2.
 */
static unsigned _calc_nr_buckets(unsigned nr_blocks)
{
	unsigned r = 8;
	unsigned n = nr_blocks / 4;

	if (n < 8)
		n = 8;

	while (r < n)
		r <<= 1;

	return r;
}

static bool _hash_table_init(struct bcache *cache, unsigned nr_entries)
{
	unsigned i;

	cache->nr_buckets = _calc_nr_buckets(nr_entries);
	cache->hash_mask = cache->nr_buckets - 1;
	cache->buckets = dm_malloc(cache->nr_buckets * sizeof(*cache->buckets));
	if (!cache->buckets)
		return false;

	for (i = 0; i < cache->nr_buckets; i++)
		dm_list_init(cache->buckets + i);

	return true;
}

static void _hash_table_exit(struct bcache *cache)
{
	dm_free(cache->buckets);
}

//----------------------------------------------------------------

static bool _init_free_list(struct bcache *cache, unsigned count)
{
	unsigned i;
	size_t block_size = cache->block_sectors << SECTOR_SHIFT;
	unsigned char *data =
		(unsigned char *) _alloc_aligned(count * block_size, PAGE_SIZE);

	/* Allocate the data for each block.  We page align the data. */
	if (!data)
		return false;

	cache->raw_data = data;
	cache->raw_blocks = dm_malloc(count * sizeof(*cache->raw_blocks));

	if (!cache->raw_blocks)
		dm_free(cache->raw_data);

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
	dm_free(cache->raw_data);
	dm_free(cache->raw_blocks);
}

static struct block *_alloc_block(struct bcache *cache)
{
	if (dm_list_empty(&cache->free))
		return NULL;

	return dm_list_struct_base(_list_pop(&cache->free), struct block, list);
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

	if (b->error)
		dm_list_add(&cache->errored, &b->list);

	else {
		_clear_flags(b, BF_DIRTY);
		_link_block(b);
	}
}

/*
 * |b->list| should be valid (either pointing to itself, on one of the other
 * lists.
 */
static bool _issue_low_level(struct block *b, enum dir d)
{
	struct bcache *cache = b->cache;
	sector_t sb = b->index * cache->block_sectors;
	sector_t se = sb + cache->block_sectors;

	if (_test_flags(b, BF_IO_PENDING))
		return false;

	_set_flags(b, BF_IO_PENDING);
	if (!_engine_issue(cache->engine, d, b->fd, sb, se, b->data, b)) {
		_complete_io(b, -EIO);
		return false;
	}

	return true;

}

static inline bool _issue_read(struct block *b)
{
	return _issue_low_level(b, DIR_READ);
}

static inline bool _issue_write(struct block *b)
{
	return _issue_low_level(b, DIR_WRITE);
}

static bool _wait_io(struct bcache *cache)
{
	return _engine_wait(cache->engine, _complete_io);
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
			_hash_remove(b);
			return b;
		}
	}

	return NULL;
}

static struct block *_new_block(struct bcache *cache, int fd, block_address index)
{
	struct block *b;

	b = _alloc_block(cache);
	while (!b && cache->nr_locked < cache->nr_cache_blocks) {
		b = _find_unused_clean_block(cache);
		if (!b) {
			if (dm_list_empty(&cache->io_pending))
				_writeback(cache, 16);
			_wait_io(cache);
		}
	}

	if (b) {
		dm_list_init(&b->list);
		dm_list_init(&b->hash);
		b->flags = 0;
		b->fd = fd;
		b->index = index;
		b->ref_count = 0;
		b->error = 0;

		_hash_insert(b);
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
				  	   int fd, block_address index,
					   unsigned flags)
{
	struct block *b = _hash_lookup(cache, fd, index);

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

		b = _new_block(cache, fd, index);
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

	if (b && !b->error) {
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
struct bcache *bcache_create(sector_t block_sectors, unsigned nr_cache_blocks)
{
	struct bcache *cache;

	if (!nr_cache_blocks) {
		log_warn("bcache must have at least one cache block");
		return NULL;
	}

	if (!block_sectors) {
		log_warn("bcache must have a non zero block size");
		return NULL;
	}

	if (block_sectors & ((PAGE_SIZE >> SECTOR_SHIFT) - 1)) {
		log_warn("bcache block size must be a multiple of page size");
		return NULL;
	}

	cache = dm_malloc(sizeof(*cache));
	if (!cache)
		return NULL;

	cache->block_sectors = block_sectors;
	cache->nr_cache_blocks = nr_cache_blocks;
	cache->max_io = nr_cache_blocks < MAX_IO ? nr_cache_blocks : MAX_IO;
	cache->engine = _engine_create(cache->max_io);
	if (!cache->engine) {
		dm_free(cache);
		return NULL;
	}

	cache->nr_locked = 0;
	cache->nr_dirty = 0;
	cache->nr_io_pending = 0;

	dm_list_init(&cache->free);
	dm_list_init(&cache->errored);
	dm_list_init(&cache->dirty);
	dm_list_init(&cache->clean);
	dm_list_init(&cache->io_pending);

	if (!_hash_table_init(cache, nr_cache_blocks)) {
		_engine_destroy(cache->engine);
		dm_free(cache);
		return NULL;
	}

	cache->read_hits = 0;
	cache->read_misses = 0;
	cache->write_zeroes = 0;
	cache->write_hits = 0;
	cache->write_misses = 0;
	cache->prefetches = 0;

	if (!_init_free_list(cache, nr_cache_blocks)) {
		_engine_destroy(cache->engine);
		_hash_table_exit(cache);
		dm_free(cache);
		return NULL;
	}

	return cache;
}

void bcache_destroy(struct bcache *cache)
{
	if (cache->nr_locked)
		log_warn("some blocks are still locked");

	bcache_flush(cache);
	_wait_all(cache);
	_exit_free_list(cache);
	_hash_table_exit(cache);
	_engine_destroy(cache->engine);
	dm_free(cache);
}

unsigned bcache_nr_cache_blocks(struct bcache *cache)
{
	return cache->nr_cache_blocks;
}

unsigned bcache_max_prefetches(struct bcache *cache)
{
	return cache->max_io;
}

void bcache_prefetch(struct bcache *cache, int fd, block_address index)
{
	struct block *b = _hash_lookup(cache, fd, index);

	if (!b) {
		b = _new_block(cache, fd, index);
		if (b && (cache->nr_io_pending < cache->max_io)) {
			cache->prefetches++;
			_issue_read(b);
		}
	}
}

bool bcache_get(struct bcache *cache, int fd, block_address index,
	        unsigned flags, struct block **result)
{
	struct block *b;

	b = _lookup_or_read_block(cache, fd, index, flags);
	if (b) {
		if (!b->ref_count)
			cache->nr_locked++;
		b->ref_count++;

		*result = b;
		return true;
	}

	*result = NULL;
	log_warn("couldn't get block");
	return false;
}

void bcache_put(struct block *b)
{
	if (!b->ref_count) {
		log_warn("ref count on bcache block already zero");
		return;
	}

	b->ref_count--;
	if (!b->ref_count)
		b->cache->nr_locked--;

	if (_test_flags(b, BF_DIRTY))
		_preemptive_writeback(b->cache);
}

int bcache_flush(struct bcache *cache)
{
	while (!dm_list_empty(&cache->dirty)) {
		struct block *b = dm_list_item(_list_pop(&cache->dirty), struct block);
		if (b->ref_count || _test_flags(b, BF_IO_PENDING))
			// The superblock may well be still locked.
			continue;

		_issue_write(b);
	}

	_wait_all(cache);

	return dm_list_empty(&cache->errored) ? 0 : -EIO;
}

static void _recycle_block(struct bcache *cache, struct block *b)
{
	_unlink_block(b);
	_hash_remove(b);
	dm_list_add(&cache->free, &b->list);
}

/*
 * You can safely call this with a NULL block.
 */
static void _invalidate_block(struct bcache *cache, struct block *b)
{
	if (!b)
		return;

	if (_test_flags(b, BF_IO_PENDING))
		_wait_specific(b);

	if (b->ref_count)
		log_warn("bcache_invalidate: block (%d, %llu) still held",
			 b->fd, (unsigned long long) index);
	else {
		if (_test_flags(b, BF_DIRTY)) {
			_issue_write(b);
			_wait_specific(b);
		}

		_recycle_block(cache, b);
	}
}

void bcache_invalidate(struct bcache *cache, int fd, block_address index)
{
	_invalidate_block(cache, _hash_lookup(cache, fd, index));
}

// FIXME: switch to a trie, or maybe 1 hash table per fd?  To save iterating
// through the whole cache.
void bcache_invalidate_fd(struct bcache *cache, int fd)
{
	struct block *b, *tmp;

	// Start writing back any dirty blocks on this fd.
	dm_list_iterate_items_safe (b, tmp, &cache->dirty)
		if (b->fd == fd)
			_issue_write(b);

	_wait_all(cache);

	// Everything should be in the clean list now.
	dm_list_iterate_items_safe (b, tmp, &cache->clean)
		if (b->fd == fd)
			_invalidate_block(cache, b);

	// Except they could be in the errored list :)
	dm_list_iterate_items_safe (b, tmp, &cache->errored)
		if (b->fd == fd)
			_recycle_block(cache, b);
}

//----------------------------------------------------------------

