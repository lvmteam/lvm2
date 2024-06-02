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

//----------------------------------------------------------------

#ifdef SIMPLE_RADIX_TREE
#include "base/data-struct/radix-tree-simple.c"
#else
#include "base/data-struct/radix-tree-adaptive.c"
#endif

//----------------------------------------------------------------

struct visitor {
	struct radix_tree_iterator it;
	unsigned pos, nr_entries;
	union radix_value *values;
};

static bool _visitor(struct radix_tree_iterator *it,
		     const void *key, size_t keylen,
		     union radix_value v)
{
	struct visitor *vt = container_of(it, struct visitor, it);

	if (vt->pos >= vt->nr_entries)
		return false;

	vt->values[vt->pos++] = v;

	return true;
}

bool radix_tree_values(struct radix_tree *rt, const void *key, size_t keylen,
		       union radix_value **values, unsigned *nr_values)
{
	struct visitor vt = {
		.it.visit = _visitor,
		.nr_entries = rt->nr_entries,
		.values = calloc(rt->nr_entries + 1, sizeof(union radix_value)),
	};

	if (vt.values) {
		// build set of all values in current radix tree
		radix_tree_iterate(rt, key, keylen, &vt.it);
		*nr_values = vt.pos;
		*values = vt.values;
		return true;
	}

	return false;
}

//----------------------------------------------------------------
