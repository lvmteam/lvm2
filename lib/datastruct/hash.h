/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_HASH_H
#define _LVM_HASH_H

struct hash_table;
typedef void (*iterate_fn)(void *data);

struct hash_table *create_hash_table(unsigned size_hint);
void destroy_hash_table(struct hash_table *t);

char *hash_lookup(struct hash_table *t, const char *key);
void hash_insert(struct hash_table *t, const char *key, void *data);
void hash_remove(struct hash_table *t, const char *key);

unsigned hash_get_num_entries(struct hash_table *t);
void hash_iterate(struct hash_table *t, iterate_fn f);

#endif

