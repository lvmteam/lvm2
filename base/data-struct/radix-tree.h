// Copyright (C) 2018 Red Hat, Inc. All rights reserved.
// 
// This file is part of LVM2.
//
// This copyrighted material is made available to anyone wishing to use,
// modify, copy, or redistribute it subject to the terms and conditions
// of the GNU Lesser General Public License v.2.1.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 
#ifndef BASE_DATA_STRUCT_RADIX_TREE_H
#define BASE_DATA_STRUCT_RADIX_TREE_H

#include <stdbool.h>
#include <stdint.h>

//----------------------------------------------------------------

struct radix_tree;

union radix_value {
	void *ptr;
	uint64_t n;
};

struct radix_tree *radix_tree_create(void);

typedef void (*radix_value_dtr)(void *context, union radix_value v);

// dtr may be NULL
void radix_tree_destroy(struct radix_tree *rt, radix_value_dtr dtr, void *context);

unsigned radix_tree_size(struct radix_tree *rt);
bool radix_tree_insert(struct radix_tree *rt, uint8_t *kb, uint8_t *ke, union radix_value v);
void radix_tree_delete(struct radix_tree *rt, uint8_t *kb, uint8_t *ke);
bool radix_tree_lookup(struct radix_tree *rt,
		       uint8_t *kb, uint8_t *ke, union radix_value *result);

//----------------------------------------------------------------

#endif
