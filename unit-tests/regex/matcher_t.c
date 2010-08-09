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

#include "libdevmapper.h"
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
	char **rx = dm_malloc(sizeof(*rx) * asize);
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

			rx[nr] = dm_malloc((ptr - start) + 1);
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
		dm_free(regex[i]);

	dm_free(regex);
}

static void _scan_input(struct dm_regex *m, char **regex)
{
	char buffer[256], *ptr;
	int r;

	while (fgets(buffer, sizeof(buffer), stdin)) {
		if ((ptr = strchr(buffer, '\n')))
			*ptr = '\0';

		r = dm_regex_match(m, buffer);

		if (r >= 0)
			printf("%s : %s\n", buffer, regex[r]);
	}
}

int main(int argc, char **argv)
{
	struct dm_pool *mem;
	struct dm_regex *scanner;
	char **regex;
	int nregex;
	int ret = 0;
	int want_finger_print = 0, i;
	const char *pattern_file = NULL;

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--fingerprint"))
			want_finger_print = 1;

		else
			pattern_file = argv[i];

	if (!pattern_file) {
		fprintf(stderr, "Usage : %s [--fingerprint] <pattern_file>\n", argv[0]);
		exit(1);
	}

	dm_log_init_verbose(_LOG_DEBUG);

	if (!(mem = dm_pool_create("match_regex", 10 * 1024))) {
		fprintf(stderr, "Couldn't create pool\n");
		ret = 2;
		goto err;
	}

	if (!_read_spec(pattern_file, &regex, &nregex)) {
		fprintf(stderr, "Couldn't read the lex specification\n");
		ret = 3;
		goto err;
	}

	if (!(scanner = dm_regex_create(mem, (const char **)regex, nregex))) {
		fprintf(stderr, "Couldn't build the lexer\n");
		ret = 4;
		goto err;
	}

	if (want_finger_print)
		printf("fingerprint: %x\n", dm_regex_fingerprint(scanner));
	_scan_input(scanner, regex);
	_free_regex(regex, nregex);

    err:
	dm_pool_destroy(mem);

	return ret;
}
