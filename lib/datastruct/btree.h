/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_BTREE_H
#define _LVM_BTREE_H

#include "lvm-types.h"
#include "pool.h"

struct btree;

struct btree *btree_create(struct pool *mem);

void *btree_lookup(struct btree *t, uint32_t k);
int btree_insert(struct btree *t, uint32_t k, void *data);

struct btree_iter;
void *btree_get_data(struct btree_iter *it);

struct btree_iter *btree_first(struct btree *t);
struct btree_iter *btree_next(struct btree_iter *it);

#endif
