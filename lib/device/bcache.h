#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>

#include "libdevmapper.h"

/*----------------------------------------------------------------*/

typedef uint64_t block_address;
typedef uint64_t sector_t;

struct bcache;
struct block {
	/* clients may only access these three fields */
	int fd;
	uint64_t index;
	void *data;

	struct bcache *cache;
	struct dm_list list;
	struct dm_list hash;

	unsigned flags;
	unsigned ref_count;
	int error;
};

struct bcache *bcache_create(sector_t block_size, unsigned nr_cache_blocks);
void bcache_destroy(struct bcache *cache);

enum bcache_get_flags {
	/*
	 * The block will be zeroed before get_block returns it.  This
	 * potentially avoids a read if the block is not already in the cache.
	 * GF_DIRTY is implicit.
	 */
	GF_ZERO = (1 << 0),

	/*
	 * Indicates the caller is intending to change the data in the block, a
	 * writeback will occur after the block is released.
	 */
	GF_DIRTY = (1 << 1)
};

typedef uint64_t block_address;

unsigned bcache_get_max_prefetches(struct bcache *cache);

/*
 * Use the prefetch method to take advantage of asynchronous IO.  For example,
 * if you wanted to read a block from many devices concurrently you'd do
 * something like this:
 *
 * dm_list_iterate_items (dev, &devices)
 * 	bcache_prefetch(cache, dev->fd, block);
 *
 * dm_list_iterate_items (dev, &devices) {
 *	if (!bcache_get(cache, dev->fd, block, &b))
 *		fail();
 *
 *	process_block(b);
 * }
 *
 * It's slightly sub optimal, since you may not run the gets in the order that
 * they complete.  But we're talking a very small difference, and it's worth it
 * to keep callbacks out of this interface.
 */
void bcache_prefetch(struct bcache *cache, int fd, block_address index);

/*
 * Returns true on success.
 */
bool bcache_get(struct bcache *cache, int fd, block_address index,
	        unsigned flags, struct block **result);
void bcache_put(struct block *b);

int bcache_flush(struct bcache *cache);

/*----------------------------------------------------------------*/

#endif
