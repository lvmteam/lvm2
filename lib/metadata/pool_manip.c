/*
 * Copyright (C) 2013-2014 Red Hat, Inc. All rights reserved.
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

/*
 * This file holds common pool functions.
 */

#include "lib.h"
#include "activate.h"
#include "locking.h"
#include "metadata.h"
#include "segtype.h"
#include "lv_alloc.h"
#include "defaults.h"

int attach_pool_metadata_lv(struct lv_segment *pool_seg,
			    struct logical_volume *metadata_lv)
{
	if (!seg_is_thin_pool(pool_seg) && !seg_is_cache_pool(pool_seg)) {
		log_error(INTERNAL_ERROR
			  "Unable to attach pool metadata LV to %s segtype.",
			  pool_seg->segtype->ops->name(pool_seg));
		return 0;
	}
	pool_seg->metadata_lv = metadata_lv;
	metadata_lv->status |= seg_is_thin_pool(pool_seg) ?
		THIN_POOL_METADATA : CACHE_POOL_METADATA;
	lv_set_hidden(metadata_lv);

	return add_seg_to_segs_using_this_lv(metadata_lv, pool_seg);
}

int attach_pool_data_lv(struct lv_segment *pool_seg,
			struct logical_volume *pool_data_lv)
{
	if (!seg_is_thin_pool(pool_seg) && !seg_is_cache_pool(pool_seg)) {
		log_error(INTERNAL_ERROR
			  "Unable to attach pool data LV to %s segtype.",
			  pool_seg->segtype->ops->name(pool_seg));
		return 0;
	}

	if (!set_lv_segment_area_lv(pool_seg, 0, pool_data_lv,
				    0, seg_is_thin_pool(pool_seg) ?
				    THIN_POOL_DATA : CACHE_POOL_DATA))
		return_0;

	pool_seg->lv->status |= seg_is_thin_pool(pool_seg) ?
		THIN_POOL : CACHE_POOL;
	lv_set_hidden(pool_data_lv);

	return 1;
}

int attach_pool_lv(struct lv_segment *seg,
		   struct logical_volume *pool_lv,
		   struct logical_volume *origin,
		   struct logical_volume *merge_lv)
{
	if (!seg_is_thin_volume(seg) && !seg_is_cache(seg)) {
		log_error(INTERNAL_ERROR "Unable to attach pool to %s/%s"
			  " that is not cache or thin volume.",
			  pool_lv->vg->name, seg->lv->name);
		return 0;
	}

	seg->pool_lv = pool_lv;
	seg->origin = origin;
	seg->lv->status |= seg_is_cache(seg) ? CACHE : THIN_VOLUME;

	if (origin && !add_seg_to_segs_using_this_lv(origin, seg))
		return_0;

	if (!add_seg_to_segs_using_this_lv(pool_lv, seg))
		return_0;

	if (merge_lv) {
		if (origin != merge_lv) {
			if (!add_seg_to_segs_using_this_lv(merge_lv, seg))
				return_0;
		}

		init_snapshot_merge(seg, merge_lv);
	}

	return 1;
}

int detach_pool_lv(struct lv_segment *seg)
{
	struct lv_thin_message *tmsg, *tmp;
	struct seg_list *sl, *tsl;
	int no_update = 0;

	if (!seg->pool_lv) {
		log_error(INTERNAL_ERROR
			  "No pool associated with %s LV, %s.",
			  seg->segtype->ops->name(seg), seg->lv->name);
		return 0;
	}

	if (seg_is_cache(seg)) {
		if (!remove_seg_from_segs_using_this_lv(seg->pool_lv, seg))
			return_0;
		seg->lv->status &= ~CACHE;
		seg->pool_lv = NULL;
		return 1;
	}

	if (!lv_is_thin_pool(seg->pool_lv)) {
		log_error(INTERNAL_ERROR
			  "Cannot detach pool from LV %s.",
			  seg->lv->name);
		return 0;
	}

	/* Drop any message referencing removed segment */
	dm_list_iterate_items_safe(tmsg, tmp, &(first_seg(seg->pool_lv)->thin_messages)) {
		switch (tmsg->type) {
		case DM_THIN_MESSAGE_CREATE_SNAP:
		case DM_THIN_MESSAGE_CREATE_THIN:
			if (tmsg->u.lv == seg->lv) {
				log_debug_metadata("Discarding message for LV %s.",
						   tmsg->u.lv->name);
				dm_list_del(&tmsg->list);
				no_update = 1; /* Replacing existing */
			}
			break;
		case DM_THIN_MESSAGE_DELETE:
			if (tmsg->u.delete_id == seg->device_id) {
				log_error(INTERNAL_ERROR "Trying to delete %u again.",
					  tmsg->u.delete_id);
				return 0;
			}
			break;
		default:
			log_error(INTERNAL_ERROR "Unsupported message type %u.", tmsg->type);
			break;
		}
	}

	if (!detach_thin_external_origin(seg))
		return_0;

	if (!attach_pool_message(first_seg(seg->pool_lv),
				 DM_THIN_MESSAGE_DELETE,
				 NULL, seg->device_id, no_update))
		return_0;

	if (!remove_seg_from_segs_using_this_lv(seg->pool_lv, seg))
		return_0;

	if (seg->origin &&
	    !remove_seg_from_segs_using_this_lv(seg->origin, seg))
		return_0;

	/* If thin origin, remove it from related thin snapshots */
	/*
	 * TODO: map removal of origin as snapshot lvconvert --merge?
	 * i.e. rename thin snapshot to origin thin origin
	 */
	dm_list_iterate_items_safe(sl, tsl, &seg->lv->segs_using_this_lv) {
		if (!seg_is_thin_volume(sl->seg) ||
		    (seg->lv != sl->seg->origin))
			continue;

		if (!remove_seg_from_segs_using_this_lv(seg->lv, sl->seg))
			return_0;
		/* Thin snapshot is now regular thin volume */
		sl->seg->origin = NULL;
	}

	seg->lv->status &= ~THIN_VOLUME;
	seg->pool_lv = NULL;
	seg->origin = NULL;

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

	if ((lv_is_thin_type(seg->lv) && !seg_is_thin_pool(pool_seg)) &&
	    !seg_is_cache_pool(pool_seg)) {
		log_error("%s on %s is not a %s pool segment",
			  pool_seg->lv->name, seg->lv->name,
			  lv_is_thin_type(seg->lv) ? "thin" : "cache");
		return NULL;
	}

	return pool_seg;
}

int create_pool(struct logical_volume *pool_lv,
		const struct segment_type *segtype,
		struct alloc_handle *ah, uint32_t stripes, uint32_t stripe_size)
{
	const struct segment_type *striped;
	struct logical_volume *meta_lv, *data_lv;
	struct lv_segment *seg;
	char name[NAME_LEN];

	if (pool_lv->le_count) {
		log_error(INTERNAL_ERROR "Pool %s already has extents.",
			  pool_lv->name);
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

	if (!activation())
		log_warn("WARNING: Pool %s is created without initialization.",
			 pool_lv->name);
	else {
		if (!vg_write(pool_lv->vg) || !vg_commit(pool_lv->vg))
			return_0;

		/*
		 * If killed here, only the VISIBLE striped pool LV is left
		 * and user could easily remove it.
		 *
		 * FIXME: implement lazy clearing when activation is disabled
		 */
		/*
		 * pool_lv is a new LV so the VG lock protects us
		 * Pass in LV_TEMPORARY flag, since device is activated purely for wipe
		 * and later it is either deactivated (in cluster)
		 * or directly converted to invisible device via suspend/resume
		 */
		pool_lv->status |= LV_TEMPORARY;
		if (!activate_lv_local(pool_lv->vg->cmd, pool_lv) ||
		    /* Clear 4KB of metadata device for new thin-pool. */
		    !wipe_lv(pool_lv, (struct wipe_params) { .do_zero = 1 })) {
			log_error("Aborting. Failed to wipe pool metadata %s.",
				  pool_lv->name);
			goto bad;
		}
		pool_lv->status &= ~LV_TEMPORARY;
		/* Deactivates cleared metadata LV */
		if (!deactivate_lv_local(pool_lv->vg->cmd, pool_lv))
			goto_bad;
	}

	if (dm_snprintf(name, sizeof(name), "%s_%s", pool_lv->name,
			(segtype_is_cache_pool(segtype)) ?
			"cmeta" : "tmeta") < 0) {
		log_error("Name is too long to be a pool name.");
		goto bad;
	}

	if (!(meta_lv = lv_create_empty(name, NULL, LVM_READ | LVM_WRITE,
					ALLOC_INHERIT, pool_lv->vg)))
		goto_bad;

	if (!move_lv_segments(meta_lv, pool_lv, 0, 0))
		goto_bad;

	/* Pool data segment */
	if (!lv_add_segment(ah, 0, stripes, pool_lv, striped, stripe_size, 0, 0))
		goto_bad;

	if (!(data_lv = insert_layer_for_lv(pool_lv->vg->cmd, pool_lv,
					    pool_lv->status,
					    (segtype_is_cache_pool(segtype)) ?
					    "_cdata" : "_tdata")))
		goto_bad;

	seg = first_seg(pool_lv);
	/* Drop reference as attach_pool_data_lv() takes it again */
	if (!remove_seg_from_segs_using_this_lv(data_lv, seg))
		goto_bad;

	seg->segtype = segtype; /* Set as thin_pool or cache_pool segment */

	if (!attach_pool_data_lv(seg, data_lv))
		goto_bad;

	if (!attach_pool_metadata_lv(seg, meta_lv))
		goto_bad;

	return 1;

bad:
	if (activation()) {
		if (deactivate_lv_local(pool_lv->vg->cmd, pool_lv)) {
			log_error("Aborting. Could not deactivate pool %s.",
				  pool_lv->name);
			return 0;
		}
		if (!lv_remove(pool_lv) ||
		    !vg_write(pool_lv->vg) || !vg_commit(pool_lv->vg))
			log_error("Manual intervention may be required to "
				  "remove abandoned LV(s) before retrying.");
	}

	return 0;
}
