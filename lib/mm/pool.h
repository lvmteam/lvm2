/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_POOL_H
#define _LVM_POOL_H

#include <string.h>
#include <stdlib.h>

struct pool;

/* constructor and destructor */
struct pool *pool_create(size_t chunk_hint);
void pool_destroy(struct pool *p);

/* simple allocation/free routines */
void *pool_alloc(struct pool *p, size_t s);
void *pool_alloc_aligned(struct pool *p, size_t s, unsigned alignment);
void pool_empty(struct pool *p);
void pool_free(struct pool *p, void *ptr);

/* object building routines */
void *pool_begin_object(struct pool *p, size_t init_size);
void *pool_grow_object(struct pool *p, unsigned char *buffer, size_t delta);
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

