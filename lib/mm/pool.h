/*
 * tools/lib/pool.h
 *
 * Copyright (C) 2001 Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef POOL_H
#define POOL_H

#include <stdlib.h>

struct pool;

/* constructor and destructor */
struct pool *create_pool(size_t chunk_hint);
void destroy_pool(struct pool *p);

/* simple allocation/free routines */
void *pool_alloc(struct pool *p, size_t s);
void *pool_alloc_aligned(struct pool *p, size_t s, unsigned alignment);
void pool_free(struct pool *p, void *ptr);

/* object building routines */
void *pool_begin_object(struct pool *p, size_t hint, unsigned align);
void *pool_grow_object(struct pool *p, unsigned char *buffer, size_t n);
void *pool_end_object(struct pool *p);
void pool_abandon_object(struct pool *p);

/* utilities */
char *pool_strdup(struct pool *p, const char *str);

static inline void *pool_zalloc(struct pool *p, size_t s) {
	void *ptr = pool_alloc(p, s);

	if (ptr)
		memset(ptr, 0, s);

	return ptr;
}

#endif

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 * vim:ai cin ts=4
 */

