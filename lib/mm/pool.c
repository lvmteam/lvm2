/*
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
 *
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

/* Alignment needs to be 8 for Alpha */
#define DEFAULT_ALIGNMENT 8

struct pool *create_pool(size_t chunk_hint)
{
	size_t new_size = 1024;
	struct pool *p = dbg_malloc(sizeof(*p));

	if (!p) {
		log_err("couldn't create pool");
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

void destroy_pool(struct pool *p)
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
		_align_chunk(c, alignment);
	}

	r = c->begin;
	c->begin += s;
	return r;
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
		log_warn("pool_free asked to free a pointer "
			 "that wasn't in the pool, doing nothing");
	else
		p->chunk = c;
}

void *pool_begin_object(struct pool *p, size_t hint, unsigned align)
{
	struct chunk *c = p->chunk;

	p->object_len = 0;
	p->object_alignment = align;

	_align_chunk(c, align);
	if (c->end - c->begin < hint) {
		/* allocate a new chunk */
		c = _new_chunk(p,
			       hint > (p->chunk_size - sizeof(struct chunk)) ?
			       hint + sizeof(struct chunk) + align :
			       p->chunk_size);
		_align_chunk(c, align);
	}

	return c->begin;
}

void *pool_grow_object(struct pool *p, unsigned char *buffer, size_t n)
{
	struct chunk *c = p->chunk;

	if (c->end - (c->begin + p->object_len) < n) {
		/* move into a new chunk */
		if (p->object_len + n > (p->chunk_size / 2))
			_new_chunk(p, (p->object_len + n) * 2);
		else
			_new_chunk(p, p->chunk_size);

		_align_chunk(p->chunk, p->object_alignment);
		memcpy(p->chunk->begin, c->begin, p->object_len);
		c = p->chunk;
	}

	memcpy(c->begin + p->object_len, buffer, n);
	p->object_len += n;
	return c->begin;
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
		c = dbg_malloc(s);
		c->end = (char *) c + s;
	}

	c->prev = p->chunk;
	c->begin = (char *) (c + 1);
	p->chunk = c;

	return c;
}

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 * vim:ai cin ts=8
 */
