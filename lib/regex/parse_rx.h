/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_PARSE_REGEX_H
#define _LVM_PARSE_REGEX_H

#include "bitset.h"

enum {
	CAT,
	STAR,
	PLUS,
	OR,
	QUEST,
	CHARSET
};

/*
 * We're never going to be running the regex on non-printable
 * chars, so we can use a couple of these chars to represent the
 * start and end of a string.
 */
#define HAT_CHAR 0x2
#define DOLLAR_CHAR 0x3

struct rx_node {
	int type;
	bitset_t charset;
	struct rx_node *left, *right;

	/* used to build the dfa for the toker */
	int nullable, final;
	bitset_t firstpos;
	bitset_t lastpos;
	bitset_t followpos;
};

struct rx_node *rx_parse_str(struct pool *mem, const char *str);
struct rx_node *rx_parse_tok(struct pool *mem,
			     const char *begin, const char *end);

#endif
