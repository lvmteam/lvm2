/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 */


#include "dbg_malloc.h"
#include "hash.h"
#include "log.h"

struct hash_node {
	struct hash_node *next;
	void *data;
	char key[1];
};

struct hash_table {
	int num_nodes;
	int num_slots;
	struct hash_node **slots;
};

/* Permutation of the Integers 0 through 255 */
static unsigned char _nums[] = {
	1, 14,110, 25, 97,174,132,119,138,170,125,118, 27,233,140, 51,
	87,197,177,107,234,169, 56, 68, 30,  7,173, 73,188, 40, 36, 65,
	49,213,104,190, 57,211,148,223, 48,115, 15,  2, 67,186,210, 28,
	12,181,103, 70, 22, 58, 75, 78,183,167,238,157,124,147,172,144,
	176,161,141, 86, 60, 66,128, 83,156,241, 79, 46,168,198, 41,254,
	178, 85,253,237,250,154,133, 88, 35,206, 95,116,252,192, 54,221,
	102,218,255,240, 82,106,158,201, 61,  3, 89,  9, 42,155,159, 93,
	166, 80, 50, 34,175,195,100, 99, 26,150, 16,145,  4, 33,  8,189,
	121, 64, 77, 72,208,245,130,122,143, 55,105,134, 29,164,185,194,
	193,239,101,242,  5,171,126, 11, 74, 59,137,228,108,191,232,139,
	6, 24, 81, 20,127, 17, 91, 92,251,151,225,207, 21, 98,113,112,
	84,226, 18,214,199,187, 13, 32, 94,220,224,212,247,204,196, 43,
	249,236, 45,244,111,182,153,136,129, 90,217,202, 19,165,231, 71,
	230,142, 96,227, 62,179,246,114,162, 53,160,215,205,180, 47,109,
	44, 38, 31,149,135,  0,216, 52, 63, 23, 37, 69, 39,117,146,184,
	163,200,222,235,248,243,219, 10,152,131,123,229,203, 76,120,209
};

static struct hash_node *_create_node(const char *str)
{
	/* remember sizeof(n) includes an extra char from key[1],
	   so not adding 1 to the strlen as you would expect */
	struct hash_node *n = dbg_malloc(sizeof(*n) + strlen(str));

	if (n)
		strcpy(n->key, str);

	return n;
}

static unsigned _hash(const char *str)
{
	unsigned long int h = 0, g;
	while (*str) {
		h <<= 4;
		h += _nums[(int) *str++];
		g = h & ((unsigned long) 0xf << 16u);
		if (g) {
			h ^= g >> 16u;
			h ^= g >> 5u;
		}
	}
	return h;
}

struct hash_table *hash_create(unsigned size_hint)
{
	size_t len;
	unsigned new_size = 16u;
	struct hash_table *hc = dbg_malloc(sizeof(*hc));

	if (!hc) {
		stack;
		return 0;
	}

	memset(hc, 0, sizeof(*hc));

	/* round size hint up to a power of two */
	while (new_size < size_hint)
		new_size  = new_size << 1;

	hc->num_slots = new_size;
	len = sizeof(*(hc->slots)) * new_size;
	if (!(hc->slots = dbg_malloc(len))) {
		stack;
		goto bad;
	}
	memset(hc->slots, 0, len);
	return hc;

 bad:
	dbg_free(hc->slots);
	dbg_free(hc);
	return 0;
}

void hash_destroy(struct hash_table *t)
{
	struct hash_node *c, *n;
	int i;

	for (i = 0; i < t->num_slots; i++)
		for (c = t->slots[i]; c; c = n) {
			n = c->next;
			dbg_free(c);
		}

	dbg_free(t->slots);
	dbg_free(t);
}

static inline struct hash_node **_find(struct hash_table *t, const char *key)
{
	unsigned h = _hash(key) & (t->num_slots - 1);
	struct hash_node **c;

	for(c = &t->slots[h]; *c; c = &((*c)->next))
		if(!strcmp(key, (*c)->key))
			break;

	return c;
}

void *hash_lookup(struct hash_table *t, const char *key)
{
	struct hash_node **c = _find(t, key);
	return *c ? (*c)->data : 0;
}

int hash_insert(struct hash_table *t, const char *key, void *data)
{
	struct hash_node **c = _find(t, key);

	if(*c)
		(*c)->data = data;
	else {
		struct hash_node *n = _create_node(key);

		if (!n)
			return 0;

		n->data = data;
		n->next = 0;
		*c = n;
		t->num_nodes++;
	}

	return 1;
}

void hash_remove(struct hash_table *t, const char *key)
{
	struct hash_node **c = _find(t, key);

	if (*c) {
		struct hash_node *old = *c;
		*c = (*c)->next;
		dbg_free(old);
		t->num_nodes--;
	}
}

unsigned hash_get_num_entries(struct hash_table *t)
{
	return t->num_nodes;
}

void hash_iterate(struct hash_table *t, iterate_fn f)
{
	struct hash_node *c;
	int i;

	for (i = 0; i < t->num_slots; i++)
		for (c = t->slots[i]; c; c = c->next)
			f(c->data);
}

void hash_wipe(struct hash_table *t)
{
	struct hash_node **c, *old;
	int i;

	for (i = 0; i < t->num_slots; i++) {
		c = &t->slots[i];
		while (*c) {
			old = *c;
			*c = (*c)->next;
			dbg_free(old);
			t->num_nodes--;
		}
	}
}

char *hash_get_key(struct hash_table *t, struct hash_node *n)
{
	return n->key;
}

void *hash_get_data(struct hash_table *t, struct hash_node *n)
{
	return n->data;
}

static struct hash_node *_next_slot(struct hash_table *t, unsigned int s)
{
	struct hash_node *c = NULL;
	int i;

	for (i = s; i < t->num_slots && !c; i++)
		c = t->slots[i];

	return c;
}

struct hash_node *hash_get_first(struct hash_table *t)
{
	return _next_slot(t, 0);
}

struct hash_node *hash_get_next(struct hash_table *t, struct hash_node *n)
{
	unsigned int h = _hash(n->key) & (t->num_slots - 1);
	return n->next ? n->next : _next_slot(t, h + 1);
}

