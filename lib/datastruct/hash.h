/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_HASH_H
#define _LVM_HASH_H

#include "lvm-types.h"

struct hash_table;
struct hash_node;

typedef void (*iterate_fn)(void *data);

struct hash_table *hash_create(unsigned size_hint);
void hash_destroy(struct hash_table *t);
void hash_wipe(struct hash_table *t);

void *hash_lookup(struct hash_table *t, const char *key);
void *hash_lookup_fixed(struct hash_table *t, const char *key, uint32_t len);
int hash_insert(struct hash_table *t, const char *key, void *data);
void hash_remove(struct hash_table *t, const char *key);

unsigned hash_get_num_entries(struct hash_table *t);
void hash_iter(struct hash_table *t, iterate_fn f);

char *hash_get_key(struct hash_table *t, struct hash_node *n);
void *hash_get_data(struct hash_table *t, struct hash_node *n);
struct hash_node *hash_get_first(struct hash_table *t);
struct hash_node *hash_get_next(struct hash_table *t, struct hash_node *n);

#define hash_iterate(v, h) \
	for (v = hash_get_first(h); v; \
	     v = hash_get_next(h, v))

#endif

