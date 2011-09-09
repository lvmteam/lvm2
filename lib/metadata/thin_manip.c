/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "metadata.h"
#include "segtype.h"
#include "lv_alloc.h"

int attach_pool_metadata_lv(struct lv_segment *seg, struct logical_volume *pool_metadata_lv)
{
	seg->pool_metadata_lv = pool_metadata_lv;
	pool_metadata_lv->status |= THIN_POOL_METADATA;
        lv_set_hidden(pool_metadata_lv);

        return add_seg_to_segs_using_this_lv(pool_metadata_lv, seg);
}

int attach_pool_data_lv(struct lv_segment *seg, struct logical_volume *pool_data_lv)
{
	if (!set_lv_segment_area_lv(seg, 0, pool_data_lv, 0, THIN_POOL_DATA))
		return_0;

        lv_set_hidden(pool_data_lv);

        return 1;
}

int attach_pool_lv(struct lv_segment *seg, struct logical_volume *pool_lv)
{
	if (!lv_is_thin_pool(pool_lv)) {
		log_error(INTERNAL_ERROR "LV %s is not a thin pool",
			  pool_lv->name);
		return 0;
	}

	seg->pool_lv = pool_lv;
	seg->lv->status |= THIN_VOLUME;

        return add_seg_to_segs_using_this_lv(pool_lv, seg);
}

int detach_pool_lv(struct lv_segment *seg)
{
	if (!lv_is_thin_pool(seg->pool_lv)) {
		log_error(INTERNAL_ERROR "LV %s is not a thin pool",
			  seg->pool_lv->name);
		return 0;
	}

	return remove_seg_from_segs_using_this_lv(seg->pool_lv, seg);
}

struct lv_segment *find_pool_seg(const struct lv_segment *seg)
{
        struct lv_segment *pool_seg;

        pool_seg = get_only_segment_using_this_lv(seg->lv);

        if (!pool_seg) {
                log_error("Failed to find pool_seg for %s", seg->lv->name);
                return NULL;
        }

        if (!seg_is_thin_pool(pool_seg)) {
                log_error("%s on %s is not a pool segment",
                          pool_seg->lv->name, seg->lv->name);
                return NULL;
        }

        return pool_seg;
}
