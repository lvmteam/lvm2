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

#include "bcache.h"

// FIXME: need to define this in a common place (that doesn't pull in deps)
#ifndef SECTOR_SHIFT
#define SECTOR_SHIFT 9
#endif

//----------------------------------------------------------------

static void byte_range_to_block_range(struct bcache *cache, uint64_t start, size_t len,
				      block_address *bb, block_address *be)
{
	block_address block_size = bcache_block_sectors(cache) << SECTOR_SHIFT;
	*bb = start / block_size;
	*be = (start + len + block_size - 1) / block_size;
}

void bcache_prefetch_bytes(struct bcache *cache, int fd, uint64_t start, size_t len)
{
	block_address bb, be;

	byte_range_to_block_range(cache, start, len, &bb, &be);
	while (bb < be) {
		bcache_prefetch(cache, fd, bb);
		bb++;
	}
}

static uint64_t _min(uint64_t lhs, uint64_t rhs)
{
	if (rhs < lhs)
		return rhs;

	return lhs;
}

// These functions are all utilities, they should only use the public
// interface to bcache.
// FIXME: there's common code that can be factored out of these 3
bool bcache_read_bytes(struct bcache *cache, int fd, uint64_t start, size_t len, void *data)
{
	struct block *b;
	block_address bb, be, i;
	unsigned char *udata = data;
	uint64_t block_size = bcache_block_sectors(cache) << SECTOR_SHIFT;
	int errors = 0;

	byte_range_to_block_range(cache, start, len, &bb, &be);
	for (i = bb; i < be; i++)
		bcache_prefetch(cache, fd, i);

	for (i = bb; i < be; i++) {
		if (!bcache_get(cache, fd, i, 0, &b, NULL)) {
			errors++;
			continue;
		}

		if (i == bb) {
			uint64_t block_offset = start % block_size;
			size_t blen = _min(block_size - block_offset, len);
			memcpy(udata, ((unsigned char *) b->data) + block_offset, blen);
			len -= blen;
			udata += blen;
		} else {
			size_t blen = _min(block_size, len);
			memcpy(udata, b->data, blen);
			len -= blen;
			udata += blen;
		}

		bcache_put(b);
	}

	return errors ? false : true;
}

bool bcache_write_bytes(struct bcache *cache, int fd, uint64_t start, size_t len, void *data)
{
	struct block *b;
	block_address bb, be, i;
	unsigned char *udata = data;
	uint64_t block_size = bcache_block_sectors(cache) << SECTOR_SHIFT;
	int errors = 0;

	byte_range_to_block_range(cache, start, len, &bb, &be);
	for (i = bb; i < be; i++)
		bcache_prefetch(cache, fd, i);

	for (i = bb; i < be; i++) {
		if (!bcache_get(cache, fd, i, GF_DIRTY, &b, NULL)) {
			errors++;
			continue;
		}

		if (i == bb) {
			uint64_t block_offset = start % block_size;
			size_t blen = _min(block_size - block_offset, len);
			memcpy(((unsigned char *) b->data) + block_offset, udata, blen);
			len -= blen;
			udata += blen;
		} else {
			size_t blen = _min(block_size, len);
			memcpy(b->data, udata, blen);
			len -= blen;
			udata += blen;
		}

		bcache_put(b);
	}

	return errors ? false : true;
}

bool bcache_write_zeros(struct bcache *cache, int fd, uint64_t start, size_t len)
{
	struct block *b;
	block_address bb, be, i;
	uint64_t block_size = bcache_block_sectors(cache) << SECTOR_SHIFT;
	int errors = 0;

	byte_range_to_block_range(cache, start, len, &bb, &be);
	for (i = bb; i < be; i++)
		bcache_prefetch(cache, fd, i);

	for (i = bb; i < be; i++) {
		if (!bcache_get(cache, fd, i, GF_DIRTY, &b, NULL)) {
			errors++;
			continue;
		}

		if (i == bb) {
			uint64_t block_offset = start % block_size;
			size_t blen = _min(block_size - block_offset, len);
			memset(((unsigned char *) b->data) + block_offset, 0, blen);
			len -= blen;
		} else {
			size_t blen = _min(block_size, len);
			memset(b->data, 0, blen);
			len -= blen;
		}

		bcache_put(b);
	}

	return errors ? false : true;
}

//----------------------------------------------------------------
