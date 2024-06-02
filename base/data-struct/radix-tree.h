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
#include <stdio.h>

//----------------------------------------------------------------

struct radix_tree;

union radix_value {
	void *ptr;
	uint64_t n;
};

typedef void (*radix_value_dtr)(void *context, union radix_value v);

// dtr will be called on any deleted entries.  dtr may be NULL.
struct radix_tree *radix_tree_create(radix_value_dtr dtr, void *dtr_context);
void radix_tree_destroy(struct radix_tree *rt);

unsigned radix_tree_size(struct radix_tree *rt);
bool radix_tree_insert(struct radix_tree *rt, const void *key, size_t keylen, union radix_value v);
bool radix_tree_remove(struct radix_tree *rt, const void *key, size_t keylen);

// Returns the number of values removed
unsigned radix_tree_remove_prefix(struct radix_tree *rt, const void *prefix, size_t prefix_len);

bool radix_tree_lookup(struct radix_tree *rt, const void *key, size_t keylen,
		       union radix_value *result);

// The radix tree stores entries in lexicographical order.  Which means
// we can iterate entries, in order.  Or iterate entries with a particular
// prefix.
struct radix_tree_iterator {
	// Returns false if the iteration should end.
	bool (*visit)(struct radix_tree_iterator *it,
		      const void *key, size_t keylen, union radix_value v);
};

void radix_tree_iterate(struct radix_tree *rt, const void *key, size_t keylen,
			struct radix_tree_iterator *it);

// Alternative traversing radix_tree.
// Builds whole set all radix_tree  nr_values values.
// After use, free(values).
bool radix_tree_values(struct radix_tree *rt, const void *key, size_t keylen,
		       union radix_value **values, unsigned *nr_values);

// Checks that some constraints on the shape of the tree are
// being held.  For debug only.
bool radix_tree_is_well_formed(struct radix_tree *rt);
void radix_tree_dump(struct radix_tree *rt, FILE *out);

// Shortcut for ptr value return
// Note: if value would be NULL, it's same result for not/found case.
static inline void *radix_tree_lookup_ptr(struct radix_tree *rt, const void *key, size_t keylen)
{
	union radix_value v;
	return radix_tree_lookup(rt, key, keylen, &v) ? v.ptr : NULL;
}

static inline bool radix_tree_insert_ptr(struct radix_tree *rt, const void *key, size_t keylen, void *ptr)
{
	union radix_value v = { .ptr = ptr };
	return radix_tree_insert(rt, key, keylen, v);
}
//----------------------------------------------------------------

#endif
