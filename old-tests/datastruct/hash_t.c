/*
 * Copyright (C) 2001 Sistina Software (UK) Limited
 *
 * This file is released under the GPL.
 */

#include "hash.h"
#include "dbg_malloc.h"

#include <stdio.h>

static void _help(FILE *fp, const char *prog)
{
	fprintf(fp, "Usage : %s <table size> <num_entries>\n", prog);
}

struct key_list {
	struct key_list *next;
	char key[1];
};

static struct key_list *_create_word(int n)
{
	struct key_list *kl = dbg_malloc(sizeof(*kl) + 32);
	snprintf(kl->key, 32, "abc%ddef%d", n, n);
	kl->next = 0;
	return kl;
}

static struct key_list *_create_word_from_file(int n)
{
	char word[128], *ptr;
	struct key_list *kl;

	if (!fgets(word, sizeof(word), stdin))
		return 0;

	for (ptr = word; *ptr; ptr++) {
		if (*ptr == '\n') {
			*ptr = 0;
			break;
		}
	}

	kl = dbg_malloc(sizeof(*kl) + 32);
	snprintf(kl->key, 32, "%s", word);
	kl->next = 0;
	return kl;
}

static void _do_test(int table_size, int num_entries)
{
	int i;
	hash_table_t ht = hash_create(table_size);
	struct key_list *tmp, *key, *all = 0;

	for (i = 0; i < num_entries; i++) {
		/* make up a word */
		if (!(key = _create_word_from_file(i))) {
			log_error("Ran out of words !\n");
			exit(1);
		}

		/* insert it */
		hash_insert(ht, key->key, key);
		key->next = all;
		all = key;
	}

	for (key = all; key; key = key->next) {
		tmp = (struct key_list *) hash_lookup(ht, key->key);
		if (!tmp || (tmp != key)) {
			log_error("lookup failed\n");
			exit(1);
		}
	}

	for (key = all; key; key = tmp) {
		tmp = key->next;
		dbg_free(key);
	}

	hash_destroy(ht);
}

int main(int argc, char **argv)
{
	init_log();

	if (argc != 3) {
		_help(stderr, argv[0]);
		exit(1);
	}

	_do_test(atoi(argv[1]), atoi(argv[2]));

	dump_memory();
	fin_log();
	return 0;
}
