/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "pool.h"
#include "dbg_malloc.h"
#include "log.h"

#include <assert.h>

struct block {
	struct block *next;
	size_t size;
	char data[0];
};

struct pool {
	int object;
	struct block *blocks;
	struct block *tail;
};

/* by default things come out aligned for doubles */
#define DEFAULT_ALIGNMENT __alignof__ (double)

struct pool *pool_create(size_t chunk_hint)
{
	struct pool *mem = dbg_malloc(sizeof(*mem));

	if (!mem) {
		stack;
		return NULL;
	}

	mem->object = 0;
	mem->blocks = mem->tail = NULL;
	return mem;
}

static void _free_blocks(struct block *b)
{
	struct block *n;

	while (b) {
		n = b->next;
		dbg_free(b);
		b = n;
	}
}

void pool_destroy(struct pool *p)
{
	_free_blocks(p->blocks);
	dbg_free(p);
}

void *pool_alloc(struct pool *p, size_t s)
{
	return pool_alloc_aligned(p, s, DEFAULT_ALIGNMENT);
}

static void _append_block(struct pool *p, struct block *b)
{
	if (p->tail) {
		p->tail->next = b;
		p->tail = b;
	} else
		p->blocks = p->tail = b;
}

void *pool_alloc_aligned(struct pool *p, size_t s, unsigned alignment)
{
	/* FIXME: I'm currently ignoring the alignment arg. */
	size_t len = sizeof(struct block) + s;
	struct block *b = dbg_malloc(len);

	if (!b) {
		stack;
		return NULL;
	}

	b->next = NULL;
	b->size = s;

	_append_block(p, b);
	return &b->data[0];
}

void pool_empty(struct pool *p)
{
	_free_blocks(p->blocks);
	p->blocks = p->tail = NULL;
}

void pool_free(struct pool *p, void *ptr)
{
	struct block *b, *prev = NULL;

	for (b = p->blocks; b; b = b->next) {
		if ((void *) &b->data[0] == ptr)
			break;
		prev = b;
	}

	/*
	 * If this fires then you tried to free a
	 * pointer that either wasn't from this
	 * pool, or isn't the start of a block.
	 */
	assert(b);

	_free_blocks(b);

	if (prev) {
		p->tail = prev;
		prev->next = NULL;
	} else
		p->blocks = p->tail = NULL;
}

void *pool_begin_object(struct pool *p, size_t init_size)
{
	assert(!p->object);
	pool_alloc_aligned(p, hint);
	p->object = 1;
}

void *pool_grow_object(struct pool *p, unsigned char *buffer, size_t delta)
{
	struct block *old = p->tail, *new;

	assert(buffer == &old->data);

	if (!pool_alloc(p, old->size + n))
		return NULL;

	new = p->tail;

	memcpy(&new->data, &old->data, old->size);

	old->next = NULL;
	pool_free(p, buffer);

	_append_block(p, new);
	return &new->data;
}

void *pool_end_object(struct pool *p)
{
	assert(p->object);
	p->object = 0;
}

void pool_abandon_object(struct pool *p)
{
	assert(p->object);
	pool_free(p, &p->tail->data);
	p->object = 0;
}

char *pool_strdup(struct pool *p, const char *str)
{
	char *ret = pool_alloc(p, strlen(str) + 1);

	if (ret)
		strcpy(ret, str);

	return ret;
}
