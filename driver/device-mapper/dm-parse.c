/*
 * dm-parse.c
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This software is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 *     4/09/2001 - First version [Joe Thornber]
 */

#include "dm.h"

struct dm_table *dm_parse(extract_line_fn line_fn, void *l_private,
			  dm_error_fn err_fn, void *e_private)
{
	struct text_region line, word;
	struct dm_table *table = dm_table_create();
	struct target_type *ttype;
	offset_t start, size, high;
	char target_name[64];
	void *context;
	int last_line_good = 1, was_error = 0;

#define PARSE_ERROR {last_line_good = 0; was_error = 1; continue;}

	while (line_fn(&line, l_private)) {

		/*
		 * each line is of the format:
		 * <sector start> <length (sectors)> <target type> <args...>
		 */

		/* the line may be blank ... */
		dm_eat_space(&line);
		if (dm_empty_tok(&line) || (*line.b == '#'))
			continue;

		/* sector start */
		if (!dm_get_number(&line, &start)) {
			err_fn("expecting a number for sector start",
			       e_private);
			PARSE_ERROR;
		}

		/* length */
		if (!dm_get_number(&line, &size)) {
			err_fn("expecting a number for region length",
			       e_private);
			PARSE_ERROR;
		}

		/* target type */
		if (!dm_get_word(&line, &word)) {
			err_fn("target type missing", e_private);
			PARSE_ERROR;
		}

		/* we have to copy the target type to a C str */
		dm_txt_copy(target_name, sizeof(target_name), &word);

		/* lookup the target type */
		if (!(ttype = dm_get_target_type(target_name))) {
			err_fn("unable to find target type", e_private);
			PARSE_ERROR;
		}

		/* check there isn't a gap, but only if the last target
		   parsed ok. */
		if (last_line_good &&

		    ((table->num_targets &&
		      start != table->highs[table->num_targets - 1] + 1) ||
		     (!table->num_targets && start))) {
			err_fn("gap in target ranges", e_private);
			PARSE_ERROR;
		}

		/* build the target */
		if (ttype->ctr(table, start, size, &line, &context,
			       err_fn, e_private))
			PARSE_ERROR;

		/* no point registering the target
                   if there was an error. */
		if (was_error)
			continue;

		/* add the target to the table */
		high = start + (size - 1);
		if (dm_table_add_target(table, high, ttype, context)) {
			err_fn("internal error adding target to table",
			       e_private);
			PARSE_ERROR;
		}
	}

#undef PARSE_ERROR

	if (!was_error) {
		dm_table_complete(table);
		return table;
	}

	dm_table_destroy(table);
	return 0;
}

/*
 * convert the text in txt to an unsigned int,
 * returns 0 on failure.
 */
int dm_get_number(struct text_region *txt, unsigned int *n)
{
	char *ptr;

	dm_eat_space(txt);
	if (dm_empty_tok(txt))
		return 0;

	*n = simple_strtoul(txt->b, &ptr, 10);
	if (ptr == txt->b)
		return 0;

	txt->b = ptr;

	return 1;
}

/*
 * extracts text up to the next '\n'.
 */
int dm_get_line(struct text_region *txt, struct text_region *line)
{
	const char *ptr;

	dm_eat_space(txt);
	if (dm_empty_tok(txt))
		return 0;

	ptr = line->b = txt->b;
	while((ptr != txt->e) && (*ptr != '\n'))
		ptr++;

	txt->b = line->e = ptr;
	return 1;
}

/*
 * extracts the next non-whitespace token from the file.
 */
int dm_get_word(struct text_region *txt, struct text_region *word)
{
	const char *ptr;

	dm_eat_space(txt);

	if (dm_empty_tok(txt))
		return 0;

	word->b = txt->b;
	for (ptr = word->b = txt->b;
	     ptr != txt->e && !isspace((int) *ptr); ptr++)
		;

	word->e = txt->b = ptr;

	return 1;
}

/*
 * copy a text region into a traditional C str.
 */
void dm_txt_copy(char *dest, size_t max, struct text_region *txt)
{
	size_t len = txt->e - txt->b;
	if (len > --max)
		len = max;
	strncpy(dest, txt->b, len);
	dest[len] = '\0';
}

/*
 * skip leading whitespace
 */
void dm_eat_space(struct text_region *txt)
{
	while(txt->b != txt->e && isspace((int) *txt->b))
		(txt->b)++;
}


EXPORT_SYMBOL(dm_get_number);
EXPORT_SYMBOL(dm_get_word);
EXPORT_SYMBOL(dm_txt_copy);
EXPORT_SYMBOL(dm_eat_space);
