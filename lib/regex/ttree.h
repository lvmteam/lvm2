/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_TTREE_H
#define _LVM_TTREE_H

#include "pool.h"

struct ttree;

struct ttree *ttree_create(struct pool *mem, unsigned int klen);

void *ttree_lookup(struct ttree *tt, unsigned *key);
void *ttree_insert(struct ttree *tt, unsigned *key, void *data);

#endif
