/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_DBG_MALLOC_H
#define _LVM_DBG_MALLOC_H

#include "lvm-types.h"
#include <stdlib.h>
#include <string.h>

void *malloc_aux(size_t s, const char *file, int line);
#  define dbg_malloc(s) malloc_aux((s), __FILE__, __LINE__)

#ifdef DEBUG_MEM

void free_aux(void *p);
void *realloc_aux(void *p, unsigned int s, const char *file, int line);
int dump_memory(void);
void bounds_check(void);

#  define dbg_free(p) free_aux(p)
#  define dbg_realloc(p, s) realloc_aux(p, s, __FILE__, __LINE__)

#else

#  define dbg_free(p) free(p)
#  define dbg_realloc(p, s) realloc(p, s)
#  define dump_memory()
#  define bounds_check()

#endif

static inline char *dbg_strdup(const char *str)
{
	char *ret = dbg_malloc(strlen(str) + 1);

	if (ret)
		strcpy(ret, str);

	return ret;
}

#endif
