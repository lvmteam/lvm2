/*
 * Copyright (C) 2001 Sistina Software
 *
 * lvm is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * lvm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#ifndef _LVM_DBG_MALLOC_H
#define _LVM_DBG_MALLOC_H

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

#endif

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
