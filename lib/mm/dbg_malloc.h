/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DBG_MALLOC_H
#define _LVM_DBG_MALLOC_H

#include <stdlib.h>
#include <sys/types.h>

#ifdef DEBUG_MEM
void *malloc_aux(size_t s, const char *file, int line);
void free_aux(void *p);
void *realloc_aux(void *p, unsigned int s, const char *file, int line);
int dump_memory(void);
void bounds_check(void);

#define dbg_malloc(s) malloc_aux((s), __FILE__, __LINE__)
#define dbg_free(p) free_aux(p)
#define dbg_realloc(p, s) realloc_aux(p, s, __FILE__, __LINE__)
#else
#define dbg_malloc(s) malloc(s)
#define dbg_free(p) free(p)
#define dbg_realloc(p, s) realloc(p, s)
#define dump_memory()
#define bounds_check()
#endif

static inline char *dbg_strdup(const char *str)
{
	char *ret = dbg_malloc(strlen(str) + 1);

	if (ret)
		strcpy(ret, str);

	return ret;
}

#endif
