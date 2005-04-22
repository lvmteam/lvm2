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

struct lv_segment *alloc_lv_segment(struct pool *mem, uint32_t num_areas);
struct lv_segment *alloc_snapshot_seg(struct logical_volume *lv,
				      uint32_t allocated);

void set_lv_segment_area_pv(struct lv_segment *seg, uint32_t area_num,
			    struct physical_volume *pv, uint32_t pe);
void set_lv_segment_area_lv(struct lv_segment *seg, uint32_t area_num,
			    struct logical_volume *lv, uint32_t le);

#endif
