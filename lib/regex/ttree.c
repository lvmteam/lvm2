/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "ttree.h"
#include "pool.h"
#include "log.h"

#include <stdlib.h>

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

void *ttree_lookup(struct ttree *tt, unsigned *key)
{
	struct node *c = tt->root;
	int count = tt->klen;
	unsigned k = *key++;

	while (c) {
		if (k < c->k)
			c = c->l;

		else if (k > c->k)
			c = c->r;

		else {
			if (!--count)
				break;

			c = c->m;
			k = *key++;
		}
	}

	return c ? c->data : 0;
}

void *ttree_insert(struct ttree *tt, unsigned *key, void *data) 
{
	struct node *c = tt->root, *p = 0;
	int count = tt->klen, first = 1;
	unsigned k = *key;
	void *r = NULL;

	while (c) {
		p = c;
		if (k < c->k)
			c = c->l;

		else if (k > c->k)
			c = c->r;

		else {
			c = c->m;
			if (!--count)
				break;

			k = *++key;
		}
	}

	if (!count) {
		/* key is already in the tree */
		r = p->data;
		p->data = data;

	} else {
		/* FIXME: put this in seperate function */
		/* insert new chain of nodes */
		while (count--) {
			k = *key++;
			c = pool_alloc(tt->mem, sizeof(*c));

			if (!c) {
				stack;
				return NULL;
			}

			c->k = k;
			c->l = c->m = c->r = c->data = 0;
			if (!p)
				tt->root = c;

			else if (first) {
				if (k < p->k)
					p->l = c;

				else if (k > p->k)
					p->r = c;

				else
					p->m = c;

				first = 0;
			} else
				p->m = c;
			p = c;
		}
		c->data = data;
	}

	return r;
}
