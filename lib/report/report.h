/*
 * Copyright (C) 2002 Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 *
 */

#ifndef _LVM_REPORT_H
#define _LVM_REPORT_H

#include "metadata.h"

typedef enum { LVS = 1, PVS = 2, VGS = 4, SEGS = 8 } report_type_t;

struct field;
struct report_handle;

typedef int (*field_report_fn) (struct report_handle * dh, struct field * field,
				const void *data);

void *report_init(struct cmd_context *cmd, const char *format, const char *keys,
		  report_type_t *report_type, const char *separator,
		  int aligned, int buffered, int headings);
void report_free(void *handle);
int report_object(void *handle, struct volume_group *vg,
		  struct logical_volume *lv, struct physical_volume *pv,
		  struct lv_segment *seg);
int report_output(void *handle);

#endif
