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

int attach_pool_metadata_lv(struct lv_segment *seg, struct logical_volume *pool_metadata_lv)
{
	seg->pool_metadata_lv = pool_metadata_lv;
	pool_metadata_lv->status |= THIN_POOL_METADATA;
        lv_set_hidden(pool_metadata_lv);

        return add_seg_to_segs_using_this_lv(pool_metadata_lv, seg);
}

int attach_pool_data_lv(struct lv_segment *seg, struct logical_volume *pool_data_lv)
{
	seg->pool_data_lv = pool_data_lv;
	pool_data_lv->status |= THIN_POOL_DATA;
        lv_set_hidden(pool_data_lv);

        return add_seg_to_segs_using_this_lv(pool_data_lv, seg);
}

int attach_pool_lv(struct lv_segment *seg, struct logical_volume *pool_lv)
{
	seg->pool_lv = pool_lv;
	pool_lv->status |= THIN_POOL;

        return add_seg_to_segs_using_this_lv(pool_lv, seg);
}
