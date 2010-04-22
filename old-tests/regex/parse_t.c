/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2010 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* hack - using unexported internal function */
#include "regex/parse_rx.c"

#include <stdio.h>
#include <ctype.h>

static void _pretty_print(struct rx_node *rx, int depth)
{
	int i;
	for (i = 0; i < depth; i++)
		printf(" ");

	/* display info about the node */
	switch (rx->type) {
	case CAT:
		printf("Cat");
		break;

	case OR:
		printf("Or");
		break;

	case STAR:
		printf("Star");
		break;

	case PLUS:
		printf("Plus");
		break;

	case QUEST:
		printf("Quest");
		break;

	case CHARSET:
		printf("Charset : ");
		for (i = 0; i < 256; i++) {
			if (dm_bit(rx->charset, i) && isprint(i))
				printf("%c", (char) i);
		}
		break;

	default:
		printf("Unknown type");
	}
	printf("\n");

	if (rx->left)
		_pretty_print(rx->left, depth + 1);

	if (rx->right)
		_pretty_print(rx->right, depth + 1);
}

static void _regex_print(struct rx_node *rx, int depth)
{
	int i, numchars;
	int left_and_right = (rx->left && rx->right);

	if (left_and_right && rx->type == CAT && rx->left->type == OR)
		printf("(");

	if (rx->left)
		_regex_print(rx->left, depth + 1);

	if (left_and_right && rx->type == CAT && rx->left->type == OR)
		printf(")");

	/* display info about the node */
	switch (rx->type) {
	case CAT:
		//printf("Cat");
		break;

	case OR:
		printf("|");
		break;

	case STAR:
		printf("*");
		break;

	case PLUS:
		printf("+");
		break;

	case QUEST:
		printf("?");
		break;

	case CHARSET:
		numchars = 0;
		for (i = 0; i < 256; i++)
			if (dm_bit(rx->charset, i) && (isprint(i) || i == HAT_CHAR || i == DOLLAR_CHAR))
				numchars++;
		if (numchars == 97) {
			printf(".");
			break;
		}
		if (numchars > 1)
			printf("[");
		for (i = 0; i < 256; i++)
			if (dm_bit(rx->charset, i)) {
				if isprint(i)
					printf("%c", (char) i);
				else if (i == HAT_CHAR)
					printf("^");
				else if (i == DOLLAR_CHAR)
					printf("$");
			}
		if (numchars > 1)
			printf("]");
		break;

	default:
		fprintf(stderr, "Unknown type");
	}

	if (left_and_right && rx->type == CAT && rx->right->type == OR)
		printf("(");
	if (rx->right)
		_regex_print(rx->right, depth + 1);
	if (left_and_right && rx->type == CAT && rx->right->type == OR)
		printf(")");

	if (!depth)
		printf("\n");
}

int main(int argc, char **argv)
{
	struct dm_pool *mem;
	struct rx_node *rx;
	int regex_print = 0;
	int regex_arg = 1;

	if (argc == 3 && !strcmp(argv[1], "-r")) {
		regex_print++;
		regex_arg++;
		argc--;
	}

	if (argc != 2) {
		fprintf(stderr, "Usage : %s [-r] <regex>\n", argv[0]);
		exit(0);
	}

	dm_log_init_verbose(_LOG_DEBUG);

	if (!(mem = dm_pool_create("parse_regex", 1024))) {
		fprintf(stderr, "Couldn't create pool\n");
		exit(1);
	}

	if (!(rx = rx_parse_str(mem, argv[regex_arg]))) {
		dm_pool_destroy(mem);
		fprintf(stderr, "Couldn't parse regex\n");
		exit(1);
	}

	if (regex_print)
		_regex_print(rx, 0);
	else
		_pretty_print(rx, 0);

	dm_pool_destroy(mem);

	return 0;
}
