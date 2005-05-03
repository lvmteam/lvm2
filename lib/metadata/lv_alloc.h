/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.  
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

#ifndef _LVM_LV_ALLOC_H
#include "pool.h"

struct lv_segment *alloc_lv_segment(struct pool *mem,
				    struct segment_type *segtype,
				    struct logical_volume *lv,
				    uint32_t le, uint32_t len,
				    uint32_t status,
				    uint32_t stripe_size,
				    uint32_t area_count,
				    uint32_t area_len,
				    uint32_t chunk_size,
				    uint32_t extents_copied);

struct lv_segment *alloc_snapshot_seg(struct logical_volume *lv,
				      uint32_t allocated);

int set_lv_segment_area_pv(struct lv_segment *seg, uint32_t area_num,
			   struct physical_volume *pv, uint32_t pe);
void set_lv_segment_area_lv(struct lv_segment *seg, uint32_t area_num,
			    struct logical_volume *lv, uint32_t le);

#endif
