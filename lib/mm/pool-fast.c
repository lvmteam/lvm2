/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "dbg_malloc.h"
#include "log.h"

struct chunk {
	char *begin, *end;
	struct chunk *prev;
};

struct pool {
	struct chunk *chunk, *spare_chunk; /* spare_chunk is a one entry free
					      list to stop 'bobbling' */
	size_t chunk_size;
	size_t object_len;
	unsigned  object_alignment;
};

void _align_chunk(struct chunk *c, unsigned alignment);
struct chunk *_new_chunk(struct pool *p, size_t s);

/* by default things come out aligned for doubles */
#define DEFAULT_ALIGNMENT __alignof__ (double)

struct pool *pool_create(size_t chunk_hint)
{
	size_t new_size = 1024;
	struct pool *p = dbg_malloc(sizeof(*p));

	if (!p) {
		log_error("Couldn't create memory pool");
		return 0;
	}
	memset(p, 0, sizeof(*p));

	/* round chunk_hint up to the next power of 2 */
	p->chunk_size = chunk_hint + sizeof(struct chunk);
	while (new_size < p->chunk_size)
		new_size <<= 1;
	p->chunk_size = new_size;
	return p;
}

void pool_destroy(struct pool *p)
{
	struct chunk *c, *pr;
	dbg_free(p->spare_chunk);
	c = p->chunk;
	while (c) {
		pr = c->prev;
		dbg_free(c);
		c = pr;
	}

	dbg_free(p);
}

void *pool_alloc(struct pool *p, size_t s)
{
	return pool_alloc_aligned(p, s, DEFAULT_ALIGNMENT);
}

void *pool_alloc_aligned(struct pool *p, size_t s, unsigned alignment)
{
	struct chunk *c = p->chunk;
	void *r;

        /* realign begin */
	if (c)
		_align_chunk(c, alignment);

	/* have we got room ? */
	if(!c || (c->begin > c->end) || (c->end - c->begin < s)) {
		/* allocate new chunk */
		int needed = s + alignment + sizeof(struct chunk);
		c = _new_chunk(p, (needed > p->chunk_size) ?
			       needed : p->chunk_size);

		if (!c)
			return NULL;

		_align_chunk(c, alignment);
	}

	r = c->begin;
	c->begin += s;
	return r;
}

void pool_empty(struct pool *p)
{
	struct chunk *c;

	for (c = p->chunk; c && c->prev; c = c->prev)
		;

	if (p->chunk)
		pool_free(p, (char *) (p->chunk + 1));
}

void pool_free(struct pool *p, void *ptr)
{
	struct chunk *c = p->chunk;

	while (c) {
		if (((char *) c < (char *) ptr) &&
		    ((char *) c->end > (char *) ptr)) {
			c->begin = ptr;
			break;
		}

		if (p->spare_chunk)
			dbg_free(p->spare_chunk);
		p->spare_chunk = c;
		c = c->prev;
	}

	if (!c)
		log_debug("pool_free asked to free a pointer "
			 "that wasn't in the pool, doing nothing");
	else
		p->chunk = c;
}

int pool_begin_object(struct pool *p, size_t hint)
{
	struct chunk *c = p->chunk;
	const size_t align = DEFAULT_ALIGNMENT;

	p->object_len = 0;
	p->object_alignment = align;

	if (c)
		_align_chunk(c, align);

	if (!c || (c->begin > c->end) || (c->end - c->begin < hint)) {
		/* allocate a new chunk */
		c = _new_chunk(p,
			       hint > (p->chunk_size - sizeof(struct chunk)) ?
			       hint + sizeof(struct chunk) + align :
			       p->chunk_size);

		if (!c)
			return 0;

		_align_chunk(c, align);
	}

	return 1;
}

int pool_grow_object(struct pool *p, const void *extra, size_t n)
{
	struct chunk *c = p->chunk, *nc;

	if (c->end - (c->begin + p->object_len) < n) {
		/* move into a new chunk */
		if (p->object_len + n > (p->chunk_size / 2))
			nc = _new_chunk(p, (p->object_len + n) * 2);
		else
			nc = _new_chunk(p, p->chunk_size);

		if (!nc)
			return 0;

		_align_chunk(p->chunk, p->object_alignment);
		memcpy(p->chunk->begin, c->begin, p->object_len);
		c = p->chunk;
	}

	memcpy(c->begin + p->object_len, extra, n);
	p->object_len += n;
	return 1;
}

void *pool_end_object(struct pool *p)
{
	struct chunk *c = p->chunk;
	void *r = c->begin;
	c->begin += p->object_len;
	p->object_len = 0u;
	p->object_alignment = DEFAULT_ALIGNMENT;
	return r;
}

void pool_abandon_object(struct pool *p)
{
	p->object_len = 0;
	p->object_alignment = DEFAULT_ALIGNMENT;
}

char *pool_strdup(struct pool *p, const char *str)
{
	char *ret = pool_alloc(p, strlen(str) + 1);

	if (ret)
		strcpy(ret, str);

	return ret;
}

void _align_chunk(struct chunk *c, unsigned alignment)
{
	c->begin += alignment - ((unsigned long) c->begin & (alignment - 1));
}

struct chunk *_new_chunk(struct pool *p, size_t s)
{
	struct chunk *c;

	if (p->spare_chunk &&
	    ((p->spare_chunk->end - (char *) p->spare_chunk) >= s)) {
		/* reuse old chunk */
		c = p->spare_chunk;
		p->spare_chunk = 0;
	} else {
		if (!(c = dbg_malloc(s)))
			return NULL;

		c->end = (char *) c + s;
	}

	c->prev = p->chunk;
	c->begin = (char *) (c + 1);
	p->chunk = c;

	return c;
}

