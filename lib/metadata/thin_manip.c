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

	if (!attach_pool_message(first_seg(seg->pool_lv),
				 DM_THIN_MESSAGE_DELETE,
				 NULL, seg->device_id, 0))
		return_0;

	return remove_seg_from_segs_using_this_lv(seg->pool_lv, seg);
}

int attach_pool_message(struct lv_segment *seg, dm_thin_message_t type,
			struct logical_volume *lv, uint32_t device_id,
			int read_only)
{
	struct lv_thin_message *tmsg;

	if (!(tmsg = dm_pool_alloc(seg->lv->vg->vgmem, sizeof(*tmsg)))) {
		log_error("Failed to allocate memory for message.");
		return 0;
	}

	switch (type) {
	case DM_THIN_MESSAGE_CREATE_SNAP:
	case DM_THIN_MESSAGE_CREATE_THIN:
	case DM_THIN_MESSAGE_TRIM:
		tmsg->u.lv = lv;
		break;
	case DM_THIN_MESSAGE_DELETE:
		tmsg->u.delete_id = device_id;
		break;
	default:
		log_error(INTERNAL_ERROR "Unsupported message type %d", type);
		return 0;
	}

	tmsg->type = type;

	/* If the 1st message is add in non-read-only mode, modify transaction_id */
	if (!read_only && dm_list_empty(&seg->thin_messages))
		seg->transaction_id++;

	dm_list_add(&seg->thin_messages, &tmsg->list);

	log_debug("Added %s message",
		  (type == DM_THIN_MESSAGE_CREATE_SNAP ||
		   type == DM_THIN_MESSAGE_CREATE_THIN) ? "create" :
		  (type == DM_THIN_MESSAGE_TRIM) ? "trim" :
		  (type == DM_THIN_MESSAGE_DELETE) ? "delete" : "unknown");

	return 1;
}

int detach_pool_messages(struct lv_segment *seg)
{
	if (!lv_is_thin_pool(seg->lv)) {
		log_error(INTERNAL_ERROR "LV %s is not a thin pool.",
			  seg->lv->name);
		return 0;
	}

	dm_list_init(&seg->thin_messages);

	return 1;
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

/*
 * Find a free device_id for given thin_pool segment.
 *
 * \return
 * Free device id, or 0 if free device_id is not found.
 *
 * FIXME: Improve naive search and keep the value cached
 * and updated during VG lifetime (so no const for lv_segment)
 */
uint32_t get_free_pool_device_id(struct lv_segment *thin_pool_seg)
{
	uint32_t dev_id, max_id = 0;
	struct dm_list *h;

	if (!seg_is_thin_pool(thin_pool_seg)) {
		log_error("Segment in %s is not a thin pool segment.",
			  thin_pool_seg->lv->name);
		return 0;
	}

	dm_list_iterate(h, &thin_pool_seg->lv->segs_using_this_lv) {
		dev_id = dm_list_item(h, struct seg_list)->seg->device_id;
		if (dev_id > max_id)
			max_id = dev_id;
	}

	if (++max_id >= (1 << 24)) {
		// FIXME: try to find empty holes....
		log_error("Free device_id exhausted...");
		return 0;
	}

	log_debug("Found free pool device_id %u.", max_id);

	return max_id;
}
