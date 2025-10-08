/*
 * Copyright (C) 2015-2025 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Coverity usage:
 *
 * translate model into xml
 * cov-make-library -of coverity_model.xml coverity_model.c
 *
 * compile (using outdir 'cov'):
 * cov-build --dir=cov make CC=gcc
 *
 * analyze (aggressively, using 'cov')
 * cov-analyze --dir cov --wait-for-license --hfa --concurrency --enable-fnptr --enable-constraint-fpp --security --all --aggressiveness-level=high --field-offset-escape --user-model-file=coverity/coverity_model.xml
 *
 * generate html output (to 'html' from 'cov'):
 * cov-format-errors --dir cov  --html-output html
 */

/* Define NULL for Coverity modeling */
#define NULL ((void *)0)

/* Forward declarations */
struct lv_segment;
struct logical_volume;
struct segment_type;
struct cmd_context;
struct profile;
struct dm_pool;
struct dm_list;

/*
 * These functions never return NULL for valid LVs with segments
 */
struct lv_segment *first_seg(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	/* Model: assume lv is valid and has at least one segment */
	if (lv) {
		__coverity_read_pointee__(lv);
		return seg;
	}

	__coverity_panic__(); /* Should never happen in valid code */
}

struct lv_segment *last_seg(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	if (lv) {
		__coverity_read_pointee__(lv);
		return seg;
	}

	__coverity_panic__();
}

const char *find_config_tree_str(struct cmd_context *cmd, int id, struct profile *profile)
{
	char *str;

	__coverity_read_pointee__(cmd);

	if (str)
		return str;

	__coverity_panic__();
}

const char *find_config_tree_str_allow_empty(struct cmd_context *cmd, int id, struct profile *profile)
{
	char *str;

	__coverity_read_pointee__(cmd);

	if (str)
		return str;

	__coverity_panic__();
}

struct segment_type *init_unknown_segtype(struct cmd_context *cmd, const char *name)
{
	struct segment_type *seg_type;

	__coverity_read_pointee__(cmd);
	__coverity_read_pointee__(name);

	if (seg_type)
		return seg_type;

	__coverity_panic__();
}

/* simple_memccpy() from glibc */
void *memccpy(void *dest, const void *src, int c, unsigned long n)
{
	int success;

	__coverity_negative_sink__(n);
	__coverity_negative_sink__(c);
	__coverity_tainted_data_transitive__(dest, src);
	__coverity_write_buffer_bytes__(dest, n);

	if (!success)
		return NULL;

	return dest;
}

/* dm_pool_alloc - can return NULL on allocation failure */
void *dm_pool_alloc(struct dm_pool *p, unsigned long s)
{
	void *ptr;
	int success;

	if (!p) {
		__coverity_panic__();
	}

	__coverity_negative_sink__(s);  /* Catch negative sizes */

	if (!success)
		return NULL;

	ptr = __coverity_alloc__(s);

	/* Mark as escaped - pool memory doesn't need individual free */
	__coverity_escape__(ptr);

	return ptr;
}

/* dm_pool_zalloc - allocates and zeros */
void *dm_pool_zalloc(struct dm_pool *p, unsigned long s)
{
	void *ptr;
	int success;

	if (!p) {
		__coverity_panic__();
	}

	__coverity_negative_sink__(s);  /* Catch negative sizes */

	if (!success)
		return NULL;

	ptr = __coverity_alloc__(s);
	__coverity_writeall0__(ptr);  /* Memory is zeroed */

	/* Mark as escaped - pool memory doesn't need individual free */
	__coverity_escape__(ptr);

	return ptr;
}

/* dm_pool_strdup - duplicates string in pool */
char *dm_pool_strdup(struct dm_pool *p, const char *str)
{
	char *ptr;
	unsigned long size;
	int success;

	if (!p || !str) {
		__coverity_panic__();
	}

	__coverity_string_null_sink__(str);  /* str must be null-terminated */
	__coverity_string_size_sink__(str);  /* Coverity tracks the size */

	if (!success)
		return NULL;

	/* Allocate symbolic size - Coverity will track this appropriately */
	ptr = __coverity_alloc__(size);
	__coverity_tainted_data_transitive__(ptr, str);

	/* Mark as escaped - pool memory doesn't need individual free */
	__coverity_escape__(ptr);

	return ptr;
}

/* dm_pool_strndup - duplicates up to n characters */
char *dm_pool_strndup(struct dm_pool *p, const char *str, unsigned long n)
{
	char *ptr;
	int success;

	if (!p) {
		__coverity_panic__();
	}
	__coverity_string_size_source__(str);
	__coverity_negative_sink__(n);

	if (!success)
		return NULL;

	ptr = __coverity_alloc__(n + 1);

	__coverity_tainted_data_transitive__(ptr, str);
	__coverity_string_null_copy__(ptr, str, n);

	/* Mark as escaped - pool memory doesn't need individual free */
	__coverity_escape__(ptr);

	return ptr;
}

/* dm_pool_free - frees memory back to the pool */
void dm_pool_free(struct dm_pool *p, void *ptr)
{
	if (!p || !ptr) {
		__coverity_panic__();
	}
}

void dm_pool_empty(struct dm_pool *p)
{
	if (!p) {
		__coverity_panic__();
	}
}

/* dm_malloc - standard malloc wrapper */
void *dm_malloc_wrapper(unsigned long s, const char *file, int line)
{
	void *ptr;
	int success;

	__coverity_negative_sink__(s);

	if (!success)
		return NULL;

	ptr = __coverity_alloc__(s);

	__coverity_mark_as_afm_allocated__(ptr, AFM_free);

	return ptr;
}

/* dm_zalloc - standard calloc wrapper */
void *dm_zalloc_wrapper(unsigned long s, const char *file, int line)
{
	void *ptr;
	int success;

	__coverity_negative_sink__(s);

	if (!success)
		return NULL;

	ptr = __coverity_alloc__(s);

	__coverity_mark_as_afm_allocated__(ptr, AFM_free);
	__coverity_writeall0__(ptr);

	return ptr;
}

/* dm_free - standard free wrapper */
void dm_free_wrapper(void *ptr, const char *file, int line)
{
	if (ptr) {
		__coverity_free__(ptr);
	}
}


/* dm_list_init - initializes a list head */
void dm_list_init(struct dm_list *head)
{
	if (head) {
		__coverity_writeall__(head);
	}
}

/* dm_list_empty - checks if list is empty (never fails) */
int dm_list_empty(const struct dm_list *head)
{
	int is_empty;

	if (head)
		return is_empty;

	__coverity_panic__();
}

/* dm_list_add - adds to list (never fails) */
void dm_list_add(struct dm_list *head, struct dm_list *elem)
{
	if (!head || !elem) {
		__coverity_panic__();
	}

	/* Modification happens, but no failure */
	__coverity_writeall__(head);
	__coverity_writeall__(elem);
	__coverity_escape__(elem);
}
