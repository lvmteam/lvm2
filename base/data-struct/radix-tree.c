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

#include "radix-tree.h"

#include "base/memory/container_of.h"
#include "base/memory/zalloc.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

//----------------------------------------------------------------

enum node_type {
	UNSET = 0,
	VALUE,
	VALUE_CHAIN,
	PREFIX_CHAIN,
	NODE4,
	NODE16,
	NODE48,
	NODE256
};

struct value {
	enum node_type type;
	union radix_value value;
};

// This is used for entries that have a key which is a prefix of another key.
struct value_chain {
	union radix_value value;
	struct value child;
};

struct prefix_chain {
	struct value child;
	unsigned len;
	uint8_t prefix[0];
};

struct node4 {
	uint32_t nr_entries;
	uint8_t keys[4];
	struct value values[4];
};

struct node16 {
	uint32_t nr_entries;
	uint8_t keys[16];
	struct value values[16];
};

struct node48 {
	uint32_t nr_entries;
	uint8_t keys[256];
	struct value values[48];
};

struct node256 {
	struct value values[256];
};

struct radix_tree {
	unsigned nr_entries;
	struct value root;
};

//----------------------------------------------------------------

struct radix_tree *radix_tree_create(void)
{
	struct radix_tree *rt = malloc(sizeof(*rt));

	if (rt) {
		rt->nr_entries = 0;
		rt->root.type = UNSET;
	}

	return rt;
}

static void _free_node(struct value v, radix_value_dtr dtr, void *context)
{
	unsigned i;
	struct value_chain *vc;
	struct prefix_chain *pc;
	struct node4 *n4;
	struct node16 *n16;
	struct node48 *n48;
	struct node256 *n256;

	switch (v.type) {
	case UNSET:
		break;

	case VALUE:
		if (dtr)
			dtr(context, v.value);
		break;

	case VALUE_CHAIN:
		vc = v.value.ptr;
		if (dtr)
			dtr(context, vc->value);
		_free_node(vc->child, dtr, context);
		free(vc);
		break;

	case PREFIX_CHAIN:
		pc = v.value.ptr;
		_free_node(pc->child, dtr, context);
		free(pc);
		break;

	case NODE4:
		n4 = (struct node4 *) v.value.ptr;
		for (i = 0; i < n4->nr_entries; i++)
			_free_node(n4->values[i], dtr, context);
		free(n4);
		break;

	case NODE16:
		n16 = (struct node16 *) v.value.ptr;
		for (i = 0; i < n16->nr_entries; i++)
			_free_node(n16->values[i], dtr, context);
		free(n16);
		break;

	case NODE48:
		n48 = (struct node48 *) v.value.ptr;
		for (i = 0; i < n48->nr_entries; i++)
			_free_node(n48->values[i], dtr, context);
		free(n48);
		break;

	case NODE256:
		n256 = (struct node256 *) v.value.ptr;
		for (i = 0; i < 256; i++)
			_free_node(n256->values[i], dtr, context);
		free(n256);
		break;
	}
}

void radix_tree_destroy(struct radix_tree *rt, radix_value_dtr dtr, void *context)
{
	_free_node(rt->root, dtr, context);
	free(rt);
}

static bool _insert(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv);

static bool _insert_unset(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	unsigned len = ke - kb;

	if (!len) {
		// value
		v->type = VALUE;
		v->value = rv;
	} else {
		// prefix -> value
		struct prefix_chain *pc = zalloc(sizeof(*pc) + len);
		if (!pc)
			return false;

		pc->child.type = VALUE;
		pc->child.value = rv;
		pc->len = len;
		memcpy(pc->prefix, kb, len);
		v->type = PREFIX_CHAIN;
		v->value.ptr = pc;
	}

	return true;
}

static bool _insert_value(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	unsigned len = ke - kb;

	if (!len)
		// overwrite
		v->value = rv;

	else {
		// value_chain -> value
		struct value_chain *vc = zalloc(sizeof(*vc));
		if (!vc)
			return false;

		vc->value = v->value;
		if (!_insert(&vc->child, kb, ke, rv)) {
			free(vc);
			return false;
		}

		v->type = VALUE_CHAIN;
		v->value.ptr = vc;
	}

	return true;
}

static bool _insert_value_chain(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	struct value_chain *vc = v->value.ptr;
	return _insert(&vc->child, kb, ke, rv);
}

static unsigned min(unsigned lhs, unsigned rhs)
{
	if (lhs <= rhs)
		return lhs;
	else
		return rhs;
}

static bool _insert_prefix_chain(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	struct prefix_chain *pc = v->value.ptr;

	if (*kb == pc->prefix[0]) {
		// There's a common prefix let's split the chain into two and
		// recurse.
		struct prefix_chain *pc2;
		unsigned i, len = min(pc->len, ke - kb);

		for (i = 0; i < len; i++)
			if (kb[i] != pc->prefix[i])
				break;

		pc2 = zalloc(sizeof(*pc2) + pc->len - i);
		pc2->len = pc->len - i;
		memmove(pc2->prefix, pc->prefix + i, pc2->len);
		pc2->child = pc->child;

		// FIXME: this trashes pc so we can't back out
		pc->child.type = PREFIX_CHAIN;
		pc->child.value.ptr = pc2;
		pc->len = i;

		if (!_insert(&pc->child, kb + i, ke, rv)) {
			free(pc2);
			return false;
		}

	} else {
		// Stick an n4 in front.
		struct node4 *n4 = zalloc(sizeof(*n4));
		if (!n4)
			return false;

		n4->keys[0] = *kb;
		if (!_insert(n4->values, kb + 1, ke, rv)) {
			free(n4);
			return false;
		}

		if (pc->len) {
			n4->keys[1] = pc->prefix[0];
			if (pc->len == 1) {
				n4->values[1] = pc->child;
				free(pc);
			} else {
				memmove(pc->prefix, pc->prefix + 1, pc->len - 1);
				pc->len--;
				n4->values[1] = *v;
			}
			n4->nr_entries = 2;
		} else
			n4->nr_entries = 1;

		v->type = NODE4;
		v->value.ptr = n4;
	}

	return true;
}

static bool _insert_node4(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	struct node4 *n4 = v->value.ptr;
	if (n4->nr_entries == 4) {
		struct node16 *n16 = zalloc(sizeof(*n16));
		if (!n16)
			return false;

		n16->nr_entries = 5;
		memcpy(n16->keys, n4->keys, sizeof(n4->keys));
		memcpy(n16->values, n4->values, sizeof(n4->values));

		n16->keys[4] = *kb;
		if (!_insert(n16->values + 4, kb + 1, ke, rv)) {
			free(n16);
			return false;
		}
		free(n4);
		v->type = NODE16;
		v->value.ptr = n16;
	} else {
		n4 = v->value.ptr;
		if (!_insert(n4->values + n4->nr_entries, kb + 1, ke, rv))
			return false;

		n4->keys[n4->nr_entries] = *kb;
		n4->nr_entries++;
	}
	return true;
}

static bool _insert_node16(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	struct node16 *n16 = v->value.ptr;

	if (n16->nr_entries == 16) {
		unsigned i;
		struct node48 *n48 = zalloc(sizeof(*n48));

		if (!n48)
			return false;

		n48->nr_entries = 17;
		memset(n48->keys, 48, sizeof(n48->keys));

		for (i = 0; i < 16; i++) {
			n48->keys[n16->keys[i]] = i;
			n48->values[i] = n16->values[i];
		}

		n48->keys[*kb] = 16;
		if (!_insert(n48->values + 16, kb + 1, ke, rv)) {
			free(n48);
			return false;
		}

		free(n16);
		v->type = NODE48;
		v->value.ptr = n48;
	} else {
		if (!_insert(n16->values + n16->nr_entries, kb + 1, ke, rv))
			return false;
		n16->keys[n16->nr_entries] = *kb;
		n16->nr_entries++;
	}

	return true;
}

static bool _insert_node48(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	struct node48 *n48 = v->value.ptr;
	if (n48->nr_entries == 48) {
		unsigned i;
		struct node256 *n256 = zalloc(sizeof(*n256));
		if (!n256)
			return false;

		for (i = 0; i < 256; i++) {
			if (n48->keys[i] >= 48)
				continue;

			n256->values[i] = n48->values[n48->keys[i]];
		}

		if (!_insert(n256->values + *kb, kb + 1, ke, rv)) {
			free(n256);
			return false;
		}

		free(n48);
		v->type = NODE256;
		v->value.ptr = n256;

	} else {
		if (!_insert(n48->values + n48->nr_entries, kb + 1, ke, rv))
			return false;

		n48->keys[*kb] = n48->nr_entries;
		n48->nr_entries++;
	}

	return true;
}

static bool _insert_node256(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	struct node256 *n256 = v->value.ptr;
	if (!_insert(n256->values + *kb, kb + 1, ke, rv)) {
		n256->values[*kb].type = UNSET;
		return false;
	}

	return true;
}

// FIXME: the tree should not be touched if insert fails (eg, OOM)
static bool _insert(struct value *v, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	if (kb == ke) {
		if (v->type == UNSET) {
			v->type = VALUE;
			v->value = rv;

		} else if (v->type == VALUE) {
			v->value = rv;

		} else {
			struct value_chain *vc = zalloc(sizeof(*vc));
			if (!vc)
				return false;

			vc->value = rv;
			vc->child = *v;
			v->type = VALUE_CHAIN;
			v->value.ptr = vc;
		}
		return true;
	}

	switch (v->type) {
	case UNSET:
		return _insert_unset(v, kb, ke, rv);

	case VALUE:
		return _insert_value(v, kb, ke, rv);

	case VALUE_CHAIN:
		return _insert_value_chain(v, kb, ke, rv);

	case PREFIX_CHAIN:
		return _insert_prefix_chain(v, kb, ke, rv);

	case NODE4:
		return _insert_node4(v, kb, ke, rv);

	case NODE16:
		return _insert_node16(v, kb, ke, rv);

	case NODE48:
		return _insert_node48(v, kb, ke, rv);

	case NODE256:
		return _insert_node256(v, kb, ke, rv);
	}

	// can't get here
	return false;
}

struct lookup_result {
	struct value *v;
	uint8_t *kb;
};

static struct lookup_result _lookup_prefix(struct value *v, uint8_t *kb, uint8_t *ke)
{
	unsigned i;
	struct value_chain *vc;
	struct prefix_chain *pc;
	struct node4 *n4;
	struct node16 *n16;
	struct node48 *n48;
	struct node256 *n256;

	if (kb == ke)
		return (struct lookup_result) {.v = v, .kb = kb};

	switch (v->type) {
	case UNSET:
	case VALUE:
		break;

	case VALUE_CHAIN:
		vc = v->value.ptr;
		return _lookup_prefix(&vc->child, kb, ke);

	case PREFIX_CHAIN:
		pc = v->value.ptr;
		if (ke - kb < pc->len)
			return (struct lookup_result) {.v = v, .kb = kb};

		for (i = 0; i < pc->len; i++)
			if (kb[i] != pc->prefix[i])
				return (struct lookup_result) {.v = v, .kb = kb};

		return _lookup_prefix(&pc->child, kb + pc->len, ke);

	case NODE4:
		n4 = v->value.ptr;
		for (i = 0; i < n4->nr_entries; i++)
			if (n4->keys[i] == *kb)
				return _lookup_prefix(n4->values + i, kb + 1, ke);
		break;

	case NODE16:
		// FIXME: use binary search or simd?
		n16 = v->value.ptr;
		for (i = 0; i < n16->nr_entries; i++)
			if (n16->keys[i] == *kb)
				return _lookup_prefix(n16->values + i, kb + 1, ke);
		break;

	case NODE48:
		n48 = v->value.ptr;
		i = n48->keys[*kb];
		if (i < 48)
			return _lookup_prefix(n48->values + i, kb + 1, ke);
		break;

	case NODE256:
		n256 = v->value.ptr;
		return _lookup_prefix(n256->values + *kb, kb + 1, ke);
	}

	return (struct lookup_result) {.v = v, .kb = kb};
}

bool radix_tree_insert(struct radix_tree *rt, uint8_t *kb, uint8_t *ke, union radix_value rv)
{
	struct lookup_result lr = _lookup_prefix(&rt->root, kb, ke);
	if (_insert(lr.v, lr.kb, ke, rv)) {
		rt->nr_entries++;
		return true;
	}

	return false;
}

void radix_tree_delete(struct radix_tree *rt, uint8_t *key_begin, uint8_t *key_end)
{
	assert(0);
}

bool radix_tree_lookup(struct radix_tree *rt,
		       uint8_t *kb, uint8_t *ke, union radix_value *result)
{
	struct value_chain *vc;
	struct lookup_result lr = _lookup_prefix(&rt->root, kb, ke);
	if (lr.kb == ke) {
		switch (lr.v->type) {
		case VALUE:
			*result = lr.v->value;
			return true;

		case VALUE_CHAIN:
			vc = lr.v->value.ptr;
			*result = vc->value;
			return true;

		default:
			return false;
		}
	}

	return false;
}

//----------------------------------------------------------------
