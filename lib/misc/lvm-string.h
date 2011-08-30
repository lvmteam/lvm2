/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_STRING_H
#define _LVM_STRING_H

#include <stdio.h>
#include <stdarg.h>

#define NAME_LEN 128
#define UUID_PREFIX "LVM-"

struct pool;

int emit_to_buffer(char **buffer, size_t *size, const char *fmt, ...)
  __attribute__ ((format(printf, 3, 4)));

char *build_dm_uuid(struct dm_pool *mem, const char *lvid,
		    const char *layer);

int validate_name(const char *n);
int validate_tag(const char *n);

int apply_lvname_restrictions(const char *name);
int is_reserved_lvname(const char *name);

#endif
