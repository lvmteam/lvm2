/*
 * Copyright (C) 2001 Sistina Software
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

#ifndef _LVM_DISPLAY_METADATA_H
#define _LVM_DISPLAY_METADATA_H

#if 0
#include "metadata/metadata.h"

void pv_display_colons(pv_t * pv);
void pv_display_full(pv_t * pv);
void pv_show_short(pv_t * pv);
void pv_display_pe(pv_t * pv, pe_disk_t * pe);
void pv_display_pe_free(int pe_free, int p);
void pv_display_pe_text(pv_t * pv, pe_disk_t * pe, lv_disk_t * lvs);

static inline unsigned long get_pe_offset(ulong p, pv_t *pv)
{
        return pv->pe_start + (p * pv->pe_size);
}
#endif
#endif
