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
#include "activate.h"
#include "locking.h"
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
	struct lv_thin_message *tmsg;
	struct dm_list *l, *lt;

	if (!lv_is_thin_pool(seg->pool_lv)) {
		log_error(INTERNAL_ERROR "LV %s is not a thin pool",
			  seg->pool_lv->name);
		return 0;
	}

	/* Drop any message referencing removed segment */
	dm_list_iterate_safe(l, lt, &first_seg(seg->pool_lv)->thin_messages) {
		tmsg = dm_list_item(l, struct lv_thin_message);
		switch (tmsg->type) {
		case DM_THIN_MESSAGE_CREATE_SNAP:
		case DM_THIN_MESSAGE_CREATE_THIN:
		case DM_THIN_MESSAGE_TRIM:
			if (first_seg(tmsg->u.lv) == seg) {
				log_debug("Discarding message for LV %s.",
					  tmsg->u.lv->name);
				dm_list_del(&tmsg->list);
			}
		default:
			break;
		}
	}

	if (!attach_pool_message(first_seg(seg->pool_lv),
				 DM_THIN_MESSAGE_DELETE,
				 NULL, seg->device_id, 0))
		return_0;

	return remove_seg_from_segs_using_this_lv(seg->pool_lv, seg);
}

int attach_pool_message(struct lv_segment *seg, dm_thin_message_t type,
			struct logical_volume *lv, uint32_t delete_id,
			int read_only)
{
	struct lv_thin_message *tmsg;

	dm_list_iterate_items(tmsg, &seg->thin_messages) {
		if (tmsg->type == type) {
			switch (tmsg->type) {
			case DM_THIN_MESSAGE_CREATE_SNAP:
			case DM_THIN_MESSAGE_CREATE_THIN:
			case DM_THIN_MESSAGE_TRIM:
				if (tmsg->u.lv == lv) {
					log_error("Message referring LV %s already queued for %s.",
						  tmsg->u.lv->name, seg->lv->name);
					return 0;
				}
				break;
			case DM_THIN_MESSAGE_DELETE:
				if (tmsg->u.delete_id == delete_id) {
					log_error("Delete of device %u already queued for %s.",
						  tmsg->u.delete_id, seg->lv->name);
					return 0;
				}
				break;
			default:
				break;
			}
		}
	}

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
		tmsg->u.delete_id = delete_id;
		break;
	default:
		log_error(INTERNAL_ERROR "Unsupported message type %u.", type);
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

int extend_pool(struct logical_volume *pool_lv, const struct segment_type *segtype,
		struct alloc_handle *ah, uint32_t stripes, uint32_t stripe_size)
{
	const struct segment_type *striped;
	struct logical_volume *meta_lv, *data_lv;
	struct lv_segment *seg;
	const size_t len = strlen(pool_lv->name) + 16;
	char name[len];

	if (lv_is_thin_pool(pool_lv)) {
		log_error("Resize of pool %s not yet implemented.", pool_lv->name);
		return 0;
	}

	/* LV is not yet a pool, so it's extension from lvcreate */
	if (!(striped = get_segtype_from_string(pool_lv->vg->cmd, "striped")))
		return_0;

	if (activation() && segtype->ops->target_present &&
	    !segtype->ops->target_present(pool_lv->vg->cmd, NULL, NULL)) {
		log_error("%s: Required device-mapper target(s) not "
			  "detected in your kernel.", segtype->name);
		return 0;
	}

	/* Metadata segment */
	if (!lv_add_segment(ah, stripes, 1, pool_lv, striped, 1, 0, 0))
		return_0;

	if (activation()) {
		if (!vg_write(pool_lv->vg) || !vg_commit(pool_lv->vg))
			return_0;

		/*
		 * If killed here, only the VISIBLE striped pool LV is left
		 * and user could easily remove it.
		 *
		 * FIXME: implement lazy clearing when activation is disabled
		 */

		// FIXME: activate_lv_local_excl is actually wanted here
		if (!activate_lv_local(pool_lv->vg->cmd, pool_lv) ||
		    // FIXME: maybe -zero n  should  allow to recreate same thin pool
		    // and different option should be used for zero_new_blocks
		    /* Clear 4KB of metadata device for new thin-pool. */
		    !set_lv(pool_lv->vg->cmd, pool_lv, UINT64_C(0), 0)) {
			log_error("Aborting. Failed to wipe pool metadata %s.",
				  pool_lv->name);
			return 0;
		}

		if (!deactivate_lv_local(pool_lv->vg->cmd, pool_lv)) {
			log_error("Aborting. Could not deactivate pool metadata %s.",
				  pool_lv->name);
			return 0;
		}
	} else {
		log_error("Pool %s created without initilization.", pool_lv->name);
	}

	if (dm_snprintf(name, len, "%s_tmeta", pool_lv->name) < 0)
		return_0;

	if (!(meta_lv = lv_create_empty(name, NULL, LVM_READ | LVM_WRITE,
					ALLOC_INHERIT, pool_lv->vg)))
		return_0;

	if (!move_lv_segments(meta_lv, pool_lv, 0, 0))
		return_0;

	/* Pool data segment */
	if (!lv_add_segment(ah, 0, stripes, pool_lv, striped, stripe_size, 0, 0))
		return_0;

	if (!(data_lv = insert_layer_for_lv(pool_lv->vg->cmd, pool_lv,
					    pool_lv->status, "_tpool")))
		return_0;

	seg = first_seg(pool_lv);
	seg->segtype = segtype; /* Set as thin_pool segment */
	seg->lv->status |= THIN_POOL;

	if (!attach_pool_metadata_lv(seg, meta_lv))
		return_0;

	/* Drop reference as attach_pool_data_lv() takes it again */
	remove_seg_from_segs_using_this_lv(data_lv, seg);
	if (!attach_pool_data_lv(seg, data_lv))
		return_0;

	return 1;
}
