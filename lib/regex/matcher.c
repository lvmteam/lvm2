/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "matcher.h"
#include "parse_rx.h"
#include "log.h"
#include "ttree.h"
#include "bitset.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

struct dfa_state {
	int final;
	struct dfa_state *lookup[256];
};

struct state_queue {
	struct dfa_state *s;
	bitset_t bits;
	struct state_queue *next;
};

struct matcher {		/* Instance variables for the lexer */
	struct dfa_state *start;
	int num_nodes, nodes_entered;
	struct rx_node **nodes;
	struct pool *scratch, *mem;
};

#define TARGET_TRANS '\0'

static int _count_nodes(struct rx_node *rx)
{
	int r = 1;

	if (rx->left)
		r += _count_nodes(rx->left);

	if (rx->right)
		r += _count_nodes(rx->right);

	return r;
}

static void _fill_table(struct matcher *m, struct rx_node *rx)
{
	assert((rx->type != OR) || (rx->left && rx->right));

	if (rx->left)
		_fill_table(m, rx->left);

	if (rx->right)
		_fill_table(m, rx->right);

	m->nodes[m->nodes_entered++] = rx;
}

static void _create_bitsets(struct matcher *m)
{
	int i;

	for (i = 0; i < m->num_nodes; i++) {
		struct rx_node *n = m->nodes[i];
		n->firstpos = bitset_create(m->scratch, m->num_nodes);
		n->lastpos = bitset_create(m->scratch, m->num_nodes);
		n->followpos = bitset_create(m->scratch, m->num_nodes);
	}
}

static void _calc_functions(struct matcher *m)
{
	int i, j, final = 1;
	struct rx_node *rx, *c1, *c2;

	for (i = 0; i < m->num_nodes; i++) {
		rx = m->nodes[i];
		c1 = rx->left;
		c2 = rx->right;

		if (bit(rx->charset, TARGET_TRANS))
			rx->final = final++;

		switch (rx->type) {
		case CAT:
			if (c1->nullable)
				bit_union(rx->firstpos,
					  c1->firstpos, c2->firstpos);
			else
				bit_copy(rx->firstpos, c1->firstpos);

			if (c2->nullable)
				bit_union(rx->lastpos,
					  c1->lastpos, c2->lastpos);
			else
				bit_copy(rx->lastpos, c2->lastpos);

			rx->nullable = c1->nullable && c2->nullable;
			break;

		case PLUS:
			bit_copy(rx->firstpos, c1->firstpos);
			bit_copy(rx->lastpos, c1->lastpos);
			rx->nullable = c1->nullable;
			break;

		case OR:
			bit_union(rx->firstpos, c1->firstpos, c2->firstpos);
			bit_union(rx->lastpos, c1->lastpos, c2->lastpos);
			rx->nullable = c1->nullable || c2->nullable;
			break;

		case QUEST:
		case STAR:
			bit_copy(rx->firstpos, c1->firstpos);
			bit_copy(rx->lastpos, c1->lastpos);
			rx->nullable = 1;
			break;

		case CHARSET:
			bit_set(rx->firstpos, i);
			bit_set(rx->lastpos, i);
			rx->nullable = 0;
			break;

		default:
			log_error("Internal error: Unknown calc node type");
		}

		/*
		 * followpos has it's own switch
		 * because PLUS and STAR do the
		 * same thing.
		 */
		switch (rx->type) {
		case CAT:
			for (j = 0; j < m->num_nodes; j++) {
				if (bit(c1->lastpos, j)) {
					struct rx_node *n = m->nodes[j];
					bit_union(n->followpos,
						  n->followpos, c2->firstpos);
				}
			}
			break;

		case PLUS:
		case STAR:
			for (j = 0; j < m->num_nodes; j++) {
				if (bit(rx->lastpos, j)) {
					struct rx_node *n = m->nodes[j];
					bit_union(n->followpos,
						  n->followpos, rx->firstpos);
				}
			}
			break;
		}
	}
}

static inline struct dfa_state *_create_dfa_state(struct pool *mem)
{
	return pool_zalloc(mem, sizeof(struct dfa_state));
}

static struct state_queue *_create_state_queue(struct pool *mem,
					       struct dfa_state *dfa,
					       bitset_t bits)
{
	struct state_queue *r = pool_alloc(mem, sizeof(*r));

	if (!r) {
		stack;
		return NULL;
	}

	r->s = dfa;
	r->bits = bitset_create(mem, bits[0]);	/* first element is the size */
	bit_copy(r->bits, bits);
	r->next = 0;
	return r;
}

static int _calc_states(struct matcher *m, struct rx_node *rx)
{
	int iwidth = (m->num_nodes / BITS_PER_INT) + 1;
	struct ttree *tt = ttree_create(m->scratch, iwidth);
	struct state_queue *h, *t, *tmp;
	struct dfa_state *dfa, *ldfa;
	int i, a, set_bits = 0, count = 0;
	bitset_t bs = bitset_create(m->scratch, m->num_nodes), dfa_bits;

	if (!tt) {
		stack;
		return 0;
	}

	if (!bs) {
		stack;
		return 0;
	}

	/* create first state */
	dfa = _create_dfa_state(m->mem);
	m->start = dfa;
	ttree_insert(tt, rx->firstpos + 1, dfa);

	/* prime the queue */
	h = t = _create_state_queue(m->scratch, dfa, rx->firstpos);
	while (h) {
		/* pop state off front of the queue */
		dfa = h->s;
		dfa_bits = h->bits;
		h = h->next;

		/* iterate through all the inputs for this state */
		bit_clear_all(bs);
		for (a = 0; a < 256; a++) {
			/* iterate through all the states in firstpos */
			for (i = bit_get_first(dfa_bits);
			     i >= 0; i = bit_get_next(dfa_bits, i)) {
				if (bit(m->nodes[i]->charset, a)) {
					if (a == TARGET_TRANS)
						dfa->final = m->nodes[i]->final;

					bit_union(bs, bs,
						  m->nodes[i]->followpos);
					set_bits = 1;
				}
			}

			if (set_bits) {
				ldfa = ttree_lookup(tt, bs + 1);
				if (!ldfa) {
					/* push */
					ldfa = _create_dfa_state(m->mem);
					ttree_insert(tt, bs + 1, ldfa);
					tmp =
					    _create_state_queue(m->scratch,
								ldfa, bs);
					if (!h)
						h = t = tmp;
					else {
						t->next = tmp;
						t = tmp;
					}

					count++;
				}

				dfa->lookup[a] = ldfa;
				set_bits = 0;
				bit_clear_all(bs);
			}
		}
	}

	log_debug("Matcher built with %d dfa states", count);
	return 1;
}

struct matcher *matcher_create(struct pool *mem, const char **patterns, int num)
{
	char *all, *ptr;
	int i, len = 0;
	struct rx_node *rx;
	struct pool *scratch = pool_create(10 * 1024);
	struct matcher *m;

	if (!scratch) {
		stack;
		return NULL;
	}

	if (!(m = pool_alloc(mem, sizeof(*m)))) {
		stack;
		return NULL;
	}

	memset(m, 0, sizeof(*m));

	/* join the regexps together, delimiting with zero */
	for (i = 0; i < num; i++)
		len += strlen(patterns[i]) + 8;

	ptr = all = pool_alloc(scratch, len + 1);

	if (!all) {
		stack;
		goto bad;
	}

	for (i = 0; i < num; i++) {
		ptr += sprintf(ptr, "(.*(%s)%c)", patterns[i], TARGET_TRANS);
		if (i < (num - 1))
			*ptr++ = '|';
	}

	/* parse this expression */
	if (!(rx = rx_parse_tok(scratch, all, ptr))) {
		log_error("Couldn't parse regex");
		goto bad;
	}

	m->mem = mem;
	m->scratch = scratch;
	m->num_nodes = _count_nodes(rx);
	m->nodes = pool_alloc(scratch, sizeof(*m->nodes) * m->num_nodes);

	if (!m->nodes) {
		stack;
		goto bad;
	}

	_fill_table(m, rx);
	_create_bitsets(m);
	_calc_functions(m);
	_calc_states(m, rx);
	pool_destroy(scratch);
	m->scratch = NULL;

	return m;

      bad:
	pool_destroy(scratch);
	pool_destroy(mem);
	return NULL;
}

int matcher_run(struct matcher *m, const char *b)
{
	struct dfa_state *cs = m->start;
	int r = 0;

	for (; *b; b++) {

		if (!(cs = cs->lookup[(int) (unsigned char) *b]))
			break;

		if (cs->final && (cs->final > r))
			r = cs->final;
	}

	/* subtract 1 to get back to zero index */
	return r - 1;
}
