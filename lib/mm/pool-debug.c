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
	void *data;
};

struct pool {
	int begun;
	struct block *object;

	struct block *blocks;
	struct block *tail;
};

/* by default things come out aligned for doubles */
#define DEFAULT_ALIGNMENT __alignof__ (double)

struct pool *pool_create(size_t chunk_hint)
{
	struct pool *mem = dbg_malloc(sizeof(*mem));

	if (!mem) {
		log_error("Couldn't create memory pool (size %u)",
			  sizeof(*mem));
		return NULL;
	}

	mem->begun = 0;
	mem->object = 0;
	mem->blocks = mem->tail = NULL;
	return mem;
}

static void _free_blocks(struct block *b)
{
	struct block *n;

	while (b) {
		n = b->next;
		dbg_free(b->data);
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

static struct block *_new_block(size_t s, unsigned alignment)
{
	static char *_oom = "Out of memory";

	/* FIXME: I'm currently ignoring the alignment arg. */
	size_t len = sizeof(struct block) + s;
	struct block *b = dbg_malloc(len);

	/*
	 * Too lazy to implement alignment for debug version, and
	 * I don't think LVM will use anything but default
	 * align.
	 */
	assert(alignment == DEFAULT_ALIGNMENT);

	if (!b) {
		log_err(_oom);
		return NULL;
	}

	if (!(b->data = dbg_malloc(s))) {
		log_err(_oom);
		dbg_free(b);
		return NULL;
	}

	b->next = NULL;
	b->size = s;

	return b;
}

void *pool_alloc_aligned(struct pool *p, size_t s, unsigned alignment)
{
	struct block *b = _new_block(s, alignment);

	if (!b)
		return NULL;

	_append_block(p, b);
	return b->data;
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
		if (b->data == ptr)
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

int pool_begin_object(struct pool *p, size_t init_size)
{
	assert(!p->begun);
	p->begun = 1;
	return 1;
}

int pool_grow_object(struct pool *p, const void *buffer, size_t delta)
{
	struct block *new;
	size_t size = delta;

	assert(p->begun);

	if (p->object)
		size += p->object->size;

	if (!(new = _new_block(size, DEFAULT_ALIGNMENT))) {
		log_err("Couldn't extend object.");
		return 0;
	}

	if (p->object) {
		memcpy(new->data, p->object->data, p->object->size);
		dbg_free(p->object);
	}
	p->object = new;

	return 1;
}

void *pool_end_object(struct pool *p)
{
	assert(p->begun);
	_append_block(p, p->object);

	p->begun = 0;
	p->object = NULL;
	return p->tail->data;
}

void pool_abandon_object(struct pool *p)
{
	assert(p->begun);
	dbg_free(p->object);
	p->begun = 0;
	p->object = NULL;
}

char *pool_strdup(struct pool *p, const char *str)
{
	char *ret = pool_alloc(p, strlen(str) + 1);

	if (ret)
		strcpy(ret, str);

	return ret;
}
