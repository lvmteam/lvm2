/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "dmlib.h"
#include <sys/mman.h>

/* FIXME: thread unsafe */
static DM_LIST_INIT(_dm_pools);
void dm_pools_check_leaks(void);

#ifdef DEBUG_ENFORCE_POOL_LOCKING
#ifdef DEBUG_POOL
#error Do not use DEBUG_POOL with DEBUG_ENFORCE_POOL_LOCKING
#endif

/*
 * Use mprotect system call to ensure all locked pages are not writable.
 * Generates segmentation fault with write access to the locked pool.
 *
 * - Implementation is using posix_memalign() to get page aligned
 *   memory blocks (could be implemented also through malloc).
 * - Only pool-fast is properly handled for now.
 * - Checksum is slower compared to mprotect.
 */
static size_t pagesize = 0;
static size_t pagesize_mask = 0;
#define ALIGN_ON_PAGE(size) (((size) + (pagesize_mask)) & ~(pagesize_mask))
#endif

#ifdef DEBUG_POOL
#include "pool-debug.c"
#else
#include "pool-fast.c"
#endif

char *dm_pool_strdup(struct dm_pool *p, const char *str)
{
	char *ret = dm_pool_alloc_aligned(p, strlen(str) + 1, 2);

	if (ret)
		strcpy(ret, str);

	return ret;
}

char *dm_pool_strndup(struct dm_pool *p, const char *str, size_t n)
{
	char *ret = dm_pool_alloc_aligned(p, n + 1, 2);

	if (ret) {
		strncpy(ret, str, n);
		ret[n] = '\0';
	}

	return ret;
}

void *dm_pool_zalloc(struct dm_pool *p, size_t s)
{
	void *ptr = dm_pool_alloc(p, s);

	if (ptr)
		memset(ptr, 0, s);

	return ptr;
}

void dm_pools_check_leaks(void)
{
	struct dm_pool *p;

	if (dm_list_empty(&_dm_pools))
		return;

	log_error("You have a memory leak (not released memory pool):");
	dm_list_iterate_items(p, &_dm_pools) {
#ifdef DEBUG_POOL
		log_error(" [%p] %s (%u bytes)",
			  p->orig_pool,
			  p->name, p->stats.bytes);
#else
		log_error(" [%p] %s", p, p->name);
#endif
	}
	log_error(INTERNAL_ERROR "Unreleased memory pool(s) found.");
}

/**
 * Status of locked pool.
 *
 * \param p
 * Pool to be tested for lock status.
 *
 * \return
 * 1 when the pool is locked, 0 otherwise.
 */
int dm_pool_locked(struct dm_pool *p)
{
	return p->locked;
}

/**
 * Lock memory pool.
 *
 * \param p
 * Pool to be locked.
 *
 * \param crc
 * Bool specifies whether to store the pool crc/hash checksum.
 *
 * \return
 * 1 (success) when the pool was preperly locked, 0 otherwise.
 */
int dm_pool_lock(struct dm_pool *p, int crc)
{
	if (p->locked) {
		log_error(INTERNAL_ERROR "Pool %s is already locked.",
			  p->name);
		return 0;
	}

	if (crc)
		p->crc = _pool_crc(p);  /* Get crc for pool */

	if (!_pool_protect(p, PROT_READ)) {
		_pool_protect(p, PROT_READ | PROT_WRITE);
		return_0;
	}

	p->locked = 1;

	log_debug("Pool %s is locked.", p->name);

	return 1;
}

/**
 * Unlock memory pool.
 *
 * \param p
 * Pool to be unlocked.
 *
 * \param crc
 * Bool enables compare of the pool crc/hash with the stored value
 * at pool lock. The pool is not properly unlocked if there is a mismatch.
 *
 * \return
 * 1 (success) when the pool was properly unlocked, 0 otherwise.
 */
int dm_pool_unlock(struct dm_pool *p, int crc)
{
	if (!p->locked) {
		log_error(INTERNAL_ERROR "Pool %s is already unlocked.",
			  p->name);
		return 0;
	}

	p->locked = 0;

	if (!_pool_protect(p, PROT_READ | PROT_WRITE))
		return_0;

	log_debug("Pool %s is unlocked.", p->name);

	if (crc && (p->crc != _pool_crc(p))) {
		log_error(INTERNAL_ERROR "Pool %s crc mismatch.", p->name);
		return 0;
	}

	return 1;
}
