/*
 * Copyright (C) 2011-2013 Red Hat, Inc. All rights reserved.
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
#include "archiver.h"
#include "defaults.h"
#include "display.h"

int attach_pool_metadata_lv(struct lv_segment *pool_seg, struct logical_volume *metadata_lv)
{
	pool_seg->metadata_lv = metadata_lv;
	metadata_lv->status |= THIN_POOL_METADATA;
	lv_set_hidden(metadata_lv);

	return add_seg_to_segs_using_this_lv(metadata_lv, pool_seg);
}

int detach_pool_metadata_lv(struct lv_segment *pool_seg, struct logical_volume **metadata_lv)
{
	struct logical_volume *lv = pool_seg->metadata_lv;

	if (!lv || !lv_is_thin_pool_metadata(lv) ||
	    !remove_seg_from_segs_using_this_lv(lv, pool_seg)) {
		log_error(INTERNAL_ERROR "LV %s is invalid thin pool.", pool_seg->lv->name);
		return 0;
	}

	lv_set_visible(lv);
	lv->status &= ~THIN_POOL_METADATA;
	*metadata_lv = lv;
	pool_seg->metadata_lv = NULL;

	return 1;
}

int attach_pool_data_lv(struct lv_segment *pool_seg, struct logical_volume *pool_data_lv)
{
	if (!set_lv_segment_area_lv(pool_seg, 0, pool_data_lv, 0, THIN_POOL_DATA))
		return_0;

	lv_set_hidden(pool_data_lv);

	return 1;
}

int attach_pool_lv(struct lv_segment *seg, struct logical_volume *pool_lv,
		   struct logical_volume *origin)
{
	seg->pool_lv = pool_lv;
	seg->lv->status |= THIN_VOLUME;
	seg->origin = origin;

	if (origin && !add_seg_to_segs_using_this_lv(origin, seg))
		return_0;

	return add_seg_to_segs_using_this_lv(pool_lv, seg);
}

int detach_pool_lv(struct lv_segment *seg)
{
	struct lv_thin_message *tmsg, *tmp;
	struct seg_list *sl, *tsl;
	int no_update = 0;

	if (!seg->pool_lv || !lv_is_thin_pool(seg->pool_lv)) {
		log_error(INTERNAL_ERROR "LV %s is not a thin volume",
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

int attach_pool_message(struct lv_segment *pool_seg, dm_thin_message_t type,
			struct logical_volume *lv, uint32_t delete_id,
			int no_update)
{
	struct lv_thin_message *tmsg;

	if (!seg_is_thin_pool(pool_seg)) {
		log_error(INTERNAL_ERROR "LV %s is not pool.", pool_seg->lv->name);
		return 0;
	}

	if (pool_has_message(pool_seg, lv, delete_id)) {
		if (lv)
			log_error("Message referring LV %s already queued in pool %s.",
				  lv->name, pool_seg->lv->name);
		else
			log_error("Delete for device %u already queued in pool %s.",
				  delete_id, pool_seg->lv->name);
		return 0;
	}

	if (!(tmsg = dm_pool_alloc(pool_seg->lv->vg->vgmem, sizeof(*tmsg)))) {
		log_error("Failed to allocate memory for message.");
		return 0;
	}

	switch (type) {
	case DM_THIN_MESSAGE_CREATE_SNAP:
	case DM_THIN_MESSAGE_CREATE_THIN:
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
	if (!no_update && dm_list_empty(&pool_seg->thin_messages))
		pool_seg->transaction_id++;

	dm_list_add(&pool_seg->thin_messages, &tmsg->list);

	log_debug_metadata("Added %s message",
			   (type == DM_THIN_MESSAGE_CREATE_SNAP ||
			   type == DM_THIN_MESSAGE_CREATE_THIN) ? "create" :
			   (type == DM_THIN_MESSAGE_DELETE) ? "delete" : "unknown");

	return 1;
}

int attach_thin_external_origin(struct lv_segment *seg,
				struct logical_volume *external_lv)
{
	if (seg->external_lv) {
		log_error(INTERNAL_ERROR "LV \"%s\" already has external origin.",
			  seg->lv->name);
		return 0;
	}

	seg->external_lv = external_lv;

	if (external_lv) {
		if (!add_seg_to_segs_using_this_lv(external_lv, seg))
			return_0;

		external_lv->external_count++;

		if (external_lv->status & LVM_WRITE) {
			log_verbose("Setting logical volume \"%s\" read-only.",
				    external_lv->name);
			external_lv->status &= ~LVM_WRITE;
		}
	}

	return 1;
}

int detach_thin_external_origin(struct lv_segment *seg)
{
	if (seg->external_lv) {
		if (!lv_is_external_origin(seg->external_lv)) {
			log_error(INTERNAL_ERROR "Inconsitent external origin.");
			return 0;
		}

		if (!remove_seg_from_segs_using_this_lv(seg->external_lv, seg))
			return_0;

		seg->external_lv->external_count--;
		seg->external_lv = NULL;
	}

	return 1;
}

/*
 * Check whether pool has some message queued for LV or for device_id
 * When LV is NULL and device_id is 0 it just checks for any message.
 */
int pool_has_message(const struct lv_segment *seg,
		     const struct logical_volume *lv, uint32_t device_id)
{
	const struct lv_thin_message *tmsg;

	if (!seg_is_thin_pool(seg)) {
		log_error(INTERNAL_ERROR "LV %s is not pool.", seg->lv->name);
		return 0;
	}

	if (!lv && !device_id)
		return dm_list_empty(&seg->thin_messages);

	dm_list_iterate_items(tmsg, &seg->thin_messages) {
		switch (tmsg->type) {
		case DM_THIN_MESSAGE_CREATE_SNAP:
		case DM_THIN_MESSAGE_CREATE_THIN:
			if (tmsg->u.lv == lv)
				return 1;
			break;
		case DM_THIN_MESSAGE_DELETE:
			if (tmsg->u.delete_id == device_id)
				return 1;
			break;
		default:
			break;
		}
	}

	return 0;
}

int pool_is_active(const struct logical_volume *lv)
{
	struct lvinfo info;
	const struct seg_list *sl;

	if (!lv_is_thin_pool(lv)) {
		log_error(INTERNAL_ERROR "LV %s is not pool.", lv->name);
		return 0;
	}

	/* On clustered VG, query every related thin pool volume */
	if (vg_is_clustered(lv->vg)) {
		if (lv_is_active(lv))
			return 1;

		dm_list_iterate_items(sl, &lv->segs_using_this_lv)
			if (lv_is_active(sl->seg->lv)) {
				log_debug("Thin volume \"%s\" is active.", sl->seg->lv->name);
				return 1;
			}
	} else if (lv_info(lv->vg->cmd, lv, 1, &info, 0, 0) && info.exists)
		return 1; /* Non clustered VG - just checks for '-tpool' */

	return 0;
}

int pool_can_resize_metadata(const struct logical_volume *lv)
{
	static unsigned attr = 0U;
	struct lv_segment *seg;

	if (!lv_is_thin_pool(lv)) {
		log_error(INTERNAL_ERROR "LV %s is not thin pool.", lv->name);
		return 0;
	}

	seg = first_seg(lv);
	if ((attr == 0U) && activation() && seg->segtype &&
	    seg->segtype->ops->target_present &&
	    !seg->segtype->ops->target_present(lv->vg->cmd, NULL, &attr)) {
		log_error("%s: Required device-mapper target(s) not "
			  "detected in your kernel", seg->segtype->name);
		return 0;
	}

	return (attr & THIN_FEATURE_METADATA_RESIZE) ? 1 : 0;
}

int pool_below_threshold(const struct lv_segment *pool_seg)
{
	percent_t percent;
	int threshold = PERCENT_1 *
		find_config_tree_int(pool_seg->lv->vg->cmd, activation_thin_pool_autoextend_threshold_CFG);

	/* Data */
	if (!lv_thin_pool_percent(pool_seg->lv, 0, &percent))
		return_0;

	if (percent >= threshold)
		return_0;

	/* Metadata */
	if (!lv_thin_pool_percent(pool_seg->lv, 1, &percent))
		return_0;

	if (percent >= threshold)
		return_0;

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

struct logical_volume *find_pool_lv(struct logical_volume *lv)
{
	struct lv_segment *seg;

	if (!(seg = first_seg(lv))) {
		log_error("LV %s has no segment", lv->name);
		return NULL;
	}

	if (!(seg = find_pool_seg(seg)))
		return_NULL;

	return seg->lv;
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
	uint32_t max_id = 0;
	struct seg_list *sl;

	if (!seg_is_thin_pool(thin_pool_seg)) {
		log_error(INTERNAL_ERROR
			  "Segment in %s is not a thin pool segment.",
			  thin_pool_seg->lv->name);
		return 0;
	}

	dm_list_iterate_items(sl, &thin_pool_seg->lv->segs_using_this_lv)
		if (sl->seg->device_id > max_id)
			max_id = sl->seg->device_id;

	if (++max_id > DM_THIN_MAX_DEVICE_ID) {
		/* FIXME Find empty holes instead of aborting! */
		log_error("Cannot find free device_id.");
		return 0;
	}

	log_debug_metadata("Found free pool device_id %u.", max_id);

	return max_id;
}

// FIXME Rename this fn: it doesn't extend an already-existing pool AFAICT
int extend_pool(struct logical_volume *pool_lv, const struct segment_type *segtype,
		struct alloc_handle *ah, uint32_t stripes, uint32_t stripe_size)
{
	const struct segment_type *striped;
	struct logical_volume *meta_lv, *data_lv;
	struct lv_segment *seg;
	const size_t len = strlen(pool_lv->name) + 16;
	char name[len];

	if (pool_lv->le_count) {
		/* FIXME move code for manipulation from lv_manip.c */
		log_error(INTERNAL_ERROR "Pool %s has already extents.", pool_lv->name);
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

		/* pool_lv is a new LV so the VG lock protects us */
		if (!activate_lv_local(pool_lv->vg->cmd, pool_lv) ||
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
		log_warn("WARNING: Pool %s is created without initialization.", pool_lv->name);
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
					    pool_lv->status, "_tdata")))
		return_0;

	seg = first_seg(pool_lv);
	seg->segtype = segtype; /* Set as thin_pool segment */
	seg->lv->status |= THIN_POOL;

	if (!attach_pool_metadata_lv(seg, meta_lv))
		return_0;

	/* Drop reference as attach_pool_data_lv() takes it again */
	if (!remove_seg_from_segs_using_this_lv(data_lv, seg))
		return_0;
	if (!attach_pool_data_lv(seg, data_lv))
		return_0;

	return 1;
}

int update_pool_lv(struct logical_volume *lv, int activate)
{
	int monitored;

	if (!lv_is_thin_pool(lv)) {
		log_error(INTERNAL_ERROR "Updated LV %s is not pool.", lv->name);
		return 0;
	}

	if (dm_list_empty(&(first_seg(lv)->thin_messages)))
		return 1; /* No messages */

	if (activate) {
		/* If the pool is not active, do activate deactivate */
		if (!lv_is_active(lv)) {
			monitored = dmeventd_monitor_mode();
			init_dmeventd_monitor(DMEVENTD_MONITOR_IGNORE);
			if (!activate_lv_excl(lv->vg->cmd, lv))
				return_0;
			if (!deactivate_lv(lv->vg->cmd, lv))
				return_0;
			init_dmeventd_monitor(monitored);
		}
		/*
		 * Resume active pool to send thin messages.
		 * origin_only is used to skip check for resumed state
		 */
		else if (!resume_lv_origin(lv->vg->cmd, lv)) {
			log_error("Failed to resume %s.", lv->name);
			return 0;
		}
	}

	dm_list_init(&(first_seg(lv)->thin_messages));

	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	return 1;
}

int update_pool_params(struct cmd_context *cmd, unsigned attr, int passed_args,
		       uint32_t data_extents, uint32_t extent_size,
		       uint32_t *chunk_size, thin_discards_t *discards,
		       uint64_t *pool_metadata_size)
{
	size_t estimate_chunk_size;

	if (!(attr & THIN_FEATURE_BLOCK_SIZE) &&
	    (*chunk_size & (*chunk_size - 1))) {
		log_error("Chunk size must be a power of 2 for this thin target version.");
		return 0;
	} else if (*chunk_size & (DM_THIN_MIN_DATA_BLOCK_SIZE - 1)) {
		log_error("Chunk size must be multiple of %s.",
			  display_size(cmd, DM_THIN_MIN_DATA_BLOCK_SIZE));
		return 0;
	}

	if (!*pool_metadata_size) {
		/* Defaults to nr_pool_blocks * 64b converted to size in sectors */
		*pool_metadata_size = (uint64_t) data_extents * extent_size /
			(*chunk_size * (SECTOR_SIZE / UINT64_C(64)));
		/* Check if we could eventually use bigger chunk size */
		if (!(passed_args & PASS_ARG_CHUNK_SIZE)) {
			while ((*pool_metadata_size >
				(DEFAULT_THIN_POOL_OPTIMAL_SIZE / SECTOR_SIZE)) &&
			       (*chunk_size < DM_THIN_MAX_DATA_BLOCK_SIZE)) {
				*chunk_size <<= 1;
				*pool_metadata_size >>= 1;
			}
			log_verbose("Setting chunk size to %s.",
				    display_size(cmd, *chunk_size));
		} else if (*pool_metadata_size > (2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE)) {
			/* Suggest bigger chunk size */
			estimate_chunk_size = (uint64_t) data_extents * extent_size /
				(2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE *
				 (SECTOR_SIZE / UINT64_C(64)));
			log_warn("WARNING: Chunk size is too small for pool, suggested minimum is %s.",
				 display_size(cmd, UINT64_C(1) << (ffs(estimate_chunk_size) + 1)));
		}

		/* Round up to extent size */
		if (*pool_metadata_size % extent_size)
			*pool_metadata_size += extent_size - *pool_metadata_size % extent_size;
	} else {
		estimate_chunk_size =  (uint64_t) data_extents * extent_size /
			(*pool_metadata_size * (SECTOR_SIZE / UINT64_C(64)));
		/* Check to eventually use bigger chunk size */
		if (!(passed_args & PASS_ARG_CHUNK_SIZE)) {
			*chunk_size = estimate_chunk_size;

			if (*chunk_size < DM_THIN_MIN_DATA_BLOCK_SIZE)
				*chunk_size = DM_THIN_MIN_DATA_BLOCK_SIZE;
			else if (*chunk_size > DM_THIN_MAX_DATA_BLOCK_SIZE)
				*chunk_size = DM_THIN_MAX_DATA_BLOCK_SIZE;

			log_verbose("Setting chunk size %s.",
				    display_size(cmd, *chunk_size));
		} else if (*chunk_size < estimate_chunk_size) {
			/* Suggest bigger chunk size */
			log_warn("WARNING: Chunk size is smaller then suggested minimum size %s.",
				 display_size(cmd, estimate_chunk_size));
		}
	}

	if ((uint64_t) *chunk_size > (uint64_t) data_extents * extent_size) {
		log_error("Chunk size is bigger then pool data size.");
		return 0;
	}

	if (*pool_metadata_size > (2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE)) {
		if (passed_args & PASS_ARG_POOL_METADATA_SIZE)
			log_warn("WARNING: Maximum supported pool metadata size is %s.",
				 display_size(cmd, 2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE));
		*pool_metadata_size = 2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE;
	} else if (*pool_metadata_size < (2 * DEFAULT_THIN_POOL_MIN_METADATA_SIZE)) {
		if (passed_args & PASS_ARG_POOL_METADATA_SIZE)
			log_warn("WARNING: Minimum supported pool metadata size is %s.",
				 display_size(cmd, 2 * DEFAULT_THIN_POOL_MIN_METADATA_SIZE));
		*pool_metadata_size = 2 * DEFAULT_THIN_POOL_MIN_METADATA_SIZE;
	}

	log_verbose("Setting pool metadata size to %s.",
		    display_size(cmd, *pool_metadata_size));

	return 1;
}

int get_pool_discards(const char *str, thin_discards_t *discards)
{
	if (!strcasecmp(str, "passdown"))
		*discards = THIN_DISCARDS_PASSDOWN;
	else if (!strcasecmp(str, "nopassdown"))
		*discards = THIN_DISCARDS_NO_PASSDOWN;
	else if (!strcasecmp(str, "ignore"))
		*discards = THIN_DISCARDS_IGNORE;
	else {
		log_error("Thin pool discards type \"%s\" is unknown.", str);
		return 0;
	}

	return 1;
}

const char *get_pool_discards_name(thin_discards_t discards)
{
	switch (discards) {
	case THIN_DISCARDS_PASSDOWN:
                return "passdown";
	case THIN_DISCARDS_NO_PASSDOWN:
		return "nopassdown";
	case THIN_DISCARDS_IGNORE:
		return "ignore";
	}

	log_error(INTERNAL_ERROR "Unknown discards type encountered.");

	return "unknown";
}
