/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "ttree.h"
#include "pool.h"

struct node {
	unsigned k;
	struct node *l, *m, *r;
	void *data;
};

struct ttree {
	int klen;
	struct pool *mem;
	struct node *root;
};

static struct node **_lookup_single(struct node **c, unsigned int k)
{
	while (*c) {
		if (k < (*c)->k)
			c = &((*c)->l);

		else if (k > (*c)->k)
			c = &((*c)->r);

		else {
			c = &((*c)->m);
			break;
		}
	}

	return c;
}

void *ttree_lookup(struct ttree *tt, unsigned *key)
{
	struct node **c = &tt->root;
	int count = tt->klen;

	while (*c && count) {
		c = _lookup_single(c, *key++);
		count--;
	}

	return *c ? (*c)->data : NULL;
}

static struct node *_node(struct pool *mem, unsigned int k)
{
	struct node *n = pool_zalloc(mem, sizeof(*n));

	if (n)
		n->k = k;

	return n;
}

int ttree_insert(struct ttree *tt, unsigned int *key, void *data)
{
	struct node **c = &tt->root;
	int count = tt->klen;
	unsigned int k;

	do {
		k = *key++;
		c = _lookup_single(c, k);
		count--;

	} while (*c && count);

	if (!*c) {
		count++;

		while (count--) {
			if (!(*c = _node(tt->mem, k))) {
				stack;
				return 0;
			}

			k = *key++;

			if (count)
				c = &((*c)->m);
		}
	}
	(*c)->data = data;

	return 1;
}

struct ttree *ttree_create(struct pool *mem, unsigned int klen)
{
	struct ttree *tt;

	if (!(tt = pool_zalloc(mem, sizeof(*tt)))) {
		stack;
		return NULL;
	}

	tt->klen = klen;
	tt->mem = mem;
	return tt;
}
