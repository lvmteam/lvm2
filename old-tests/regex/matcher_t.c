/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "matcher.h"
#include "dbg_malloc.h"
#include "log.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>


static int _read_spec(const char *file, char ***regex, int *nregex)
{
	char buffer[1024], *start, *ptr;
	FILE *fp = fopen(file, "r");
	int asize = 100;
	char **rx = dbg_malloc(sizeof(*rx) * asize);
	int nr = 0;

	if (!fp)
		return 0;

	while (fgets(buffer, sizeof(buffer),fp)) {

		/* trim leading whitespace */
		for (ptr = buffer; *ptr && isspace((int) *ptr); ptr++);

		if (!*ptr || *ptr == '#')
			continue;

		if (*ptr == '\"') {
			ptr++;
			start = ptr;
			while (*ptr && *ptr != '\"') {
				if (*ptr == '\\')
					ptr++;
				ptr++;
			}

			if (!*ptr) {
				fprintf(stderr, "Formatting error : "
					"No terminating quote\n");
				return 0;
			}

			rx[nr] = dbg_malloc((ptr - start) + 1);
			strncpy(rx[nr], start, ptr - start);
			rx[nr][ptr - start] = '\0';
			nr++;
		} else {
			fprintf(stderr, "%s", ptr);
			fprintf(stderr, "Formatting error : \"<regex>\" "
				"<token_name>\n");
			return 0;
		}
	}

	*regex = rx;
	*nregex = nr;
	return 1;
}

static void _free_regex(char **regex, int nregex)
{
	int i;
	for (i = 0; i < nregex; i++)
		dbg_free(regex[i]);

	dbg_free(regex);
}

static void _scan_input(struct matcher *m, char **regex)
{
	char buffer[256], *ptr;
	int r;

	while (fgets(buffer, sizeof(buffer), stdin)) {
		if ((ptr = strchr(buffer, '\n')))
			*ptr = '\0';

		r = matcher_run(m, buffer, buffer + strlen(buffer));

		if (r >= 0)
			printf("%s : %s\n", buffer, regex[r]);
	}
}

int main(int argc, char **argv)
{
	struct pool *mem;
	struct matcher *scanner;
	char **regex;
	int nregex;

	if (argc < 2) {
		fprintf(stderr, "Usage : %s <pattern_file>\n", argv[0]);
		exit(1);
	}

	init_log(stderr);
	init_debug(_LOG_DEBUG);

	if (!(mem = pool_create(10 * 1024))) {
		fprintf(stderr, "Couldn't create pool\n");
		exit(2);
	}

	if (!_read_spec(argv[1], &regex, &nregex)) {
		fprintf(stderr, "Couldn't read the lex specification\n");
		exit(3);
	}

	if (!(scanner = matcher_create(mem, (const char **) regex, nregex))) {
		fprintf(stderr, "Couldn't build the lexer\n");
		exit(4);
	}

	_scan_input(scanner, regex);
	_free_regex(regex, nregex);
	pool_destroy(mem);

	dump_memory();
	fin_log();
	return 0;
}

