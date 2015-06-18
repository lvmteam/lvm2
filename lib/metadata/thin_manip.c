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
#include "memlock.h"
#include "metadata.h"
#include "segtype.h"
#include "defaults.h"
#include "display.h"

int attach_pool_message(struct lv_segment *pool_seg, dm_thin_message_t type,
			struct logical_volume *lv, uint32_t delete_id,
			int no_update)
{
	struct lv_thin_message *tmsg;

	if (!seg_is_thin_pool(pool_seg)) {
		log_error(INTERNAL_ERROR "Cannot attach message to non-pool LV %s.", pool_seg->lv->name);
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

	log_debug_metadata("Added %s message.",
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

int lv_is_merging_thin_snapshot(const struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);

	return (seg && seg->status & MERGING) ? 1 : 0;
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
		return !dm_list_empty(&seg->thin_messages);

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
		log_error(INTERNAL_ERROR "pool_is_active called with non-pool LV %s.", lv->name);
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

int thin_pool_feature_supported(const struct logical_volume *lv, int feature)
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

	return (attr & feature) ? 1 : 0;
}

int pool_below_threshold(const struct lv_segment *pool_seg)
{
	dm_percent_t percent;
	int threshold = DM_PERCENT_1 *
		find_config_tree_int(pool_seg->lv->vg->cmd, activation_thin_pool_autoextend_threshold_CFG,
				     lv_config_profile(pool_seg->lv));

	/* Data */
	if (!lv_thin_pool_percent(pool_seg->lv, 0, &percent))
		return_0;

	if (percent >= threshold)
		return 0;

	/* Metadata */
	if (!lv_thin_pool_percent(pool_seg->lv, 1, &percent))
		return_0;

	if (percent >= threshold)
		return 0;

	return 1;
}

/*
 * Validate given external origin could be used with thin pool
 */
int pool_supports_external_origin(const struct lv_segment *pool_seg, const struct logical_volume *external_lv)
{
	uint32_t csize = pool_seg->chunk_size;

	if (((external_lv->size < csize) || (external_lv->size % csize)) &&
	    !thin_pool_feature_supported(pool_seg->lv, THIN_FEATURE_EXTERNAL_ORIGIN_EXTEND)) {
		log_error("Can't use \"%s\" as external origin with \"%s\" pool. "
			  "Size %s is not a multiple of pool's chunk size %s.",
			  display_lvname(external_lv), display_lvname(pool_seg->lv),
			  display_size(external_lv->vg->cmd, external_lv->size),
			  display_size(external_lv->vg->cmd, csize));
		return 0;
	}

	return 1;
}

struct logical_volume *find_pool_lv(const struct logical_volume *lv)
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

static int _check_pool_create(const struct logical_volume *lv)
{
	const struct lv_thin_message *lmsg;
	struct lvinfo info;

	dm_list_iterate_items(lmsg, &first_seg(lv)->thin_messages) {
		if (lmsg->type != DM_THIN_MESSAGE_CREATE_THIN)
			continue;
		/* When creating new thin LV, check for size would be needed */
		if (!lv_info(lv->vg->cmd, lv, 1, &info, 0, 0) ||
		    !info.exists) {
			log_error("Pool %s needs to be locally active for threshold check.",
				  display_lvname(lv));
			return 0;
		}
		if (!pool_below_threshold(first_seg(lv))) {
			log_error("Free space in pool %s is above threshold, new volumes are not allowed.",
				  display_lvname(lv));
			return 0;
		}
		break;
	}

	return 1;
}

int update_pool_lv(struct logical_volume *lv, int activate)
{
	int monitored;
	int ret = 1;

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
			if (!activate_lv_excl(lv->vg->cmd, lv)) {
				init_dmeventd_monitor(monitored);
				return_0;
			}
			if (!lv_is_active(lv)) {
				init_dmeventd_monitor(monitored);
				log_error("Cannot activate thin pool %s, perhaps skipped in lvm.conf volume_list?",
					  display_lvname(lv));
				return 0;
			}

			if (!(ret = _check_pool_create(lv)))
				stack;

			if (!deactivate_lv(lv->vg->cmd, lv)) {
				init_dmeventd_monitor(monitored);
				return_0;
			}
			init_dmeventd_monitor(monitored);

			/* Unlock memory if possible */
			memlock_unlock(lv->vg->cmd);
		}
		/*
		 * Resume active pool to send thin messages.
		 * origin_only is used to skip check for resumed state
		 */
		else if (!resume_lv_origin(lv->vg->cmd, lv)) {
			log_error("Failed to resume %s.", lv->name);
			return 0;
		} else if (!(ret = _check_pool_create(lv)))
			stack;
	}

	dm_list_init(&(first_seg(lv)->thin_messages));

	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	return ret;
}

/* Estimate thin pool chunk size from data and metadata size (in sector units) */
static size_t _estimate_chunk_size(uint64_t data_size, uint64_t metadata_size, int attr)
{
	/*
	 * nr_pool_blocks = data_size / metadata_size
	 * chunk_size = nr_pool_blocks * 64b / sector_size
	 */
	size_t chunk_size = data_size / (metadata_size * (SECTOR_SIZE / 64));

	if (attr & THIN_FEATURE_BLOCK_SIZE) {
		/* Round up to 64KB */
		chunk_size += DM_THIN_MIN_DATA_BLOCK_SIZE - 1;
		chunk_size &= ~(size_t)(DM_THIN_MIN_DATA_BLOCK_SIZE - 1);
	} else {
		/* Round up to nearest power of 2 */
		chunk_size--;
		chunk_size |= chunk_size >> 1;
		chunk_size |= chunk_size >> 2;
		chunk_size |= chunk_size >> 4;
		chunk_size |= chunk_size >> 8;
		chunk_size |= chunk_size >> 16;
		chunk_size++;
	}

	return chunk_size;
}

int update_thin_pool_params(const struct segment_type *segtype,
			    struct volume_group *vg,
			    unsigned attr, int passed_args,
			    uint32_t pool_data_extents,
			    uint32_t *pool_metadata_extents,
			    int *chunk_size_calc_method, uint32_t *chunk_size,
			    thin_discards_t *discards, int *zero)
{
	struct cmd_context *cmd = vg->cmd;
	struct profile *profile = vg->profile;
	uint32_t extent_size = vg->extent_size;
	uint64_t pool_metadata_size = (uint64_t) *pool_metadata_extents * extent_size;
	size_t estimate_chunk_size;
	const char *str;

	if (!(passed_args & PASS_ARG_CHUNK_SIZE)) {
		if (!(*chunk_size = find_config_tree_int(cmd, allocation_thin_pool_chunk_size_CFG, profile) * 2)) {
			if (!(str = find_config_tree_str(cmd, allocation_thin_pool_chunk_size_policy_CFG, profile))) {
				log_error(INTERNAL_ERROR "Could not find configuration.");
				return 0;
			}
			if (!strcasecmp(str, "generic"))
				*chunk_size_calc_method = THIN_CHUNK_SIZE_CALC_METHOD_GENERIC;
			else if (!strcasecmp(str, "performance"))
				*chunk_size_calc_method = THIN_CHUNK_SIZE_CALC_METHOD_PERFORMANCE;
			else {
				log_error("Thin pool chunk size calculation policy \"%s\" is unrecognised.", str);
				return 0;
			}
			if (!(*chunk_size = get_default_allocation_thin_pool_chunk_size_CFG(cmd, profile)))
				return_0;
		}
	}

	if (!validate_pool_chunk_size(cmd, segtype, *chunk_size))
		return_0;

	if (!(passed_args & PASS_ARG_DISCARDS)) {
		if (!(str = find_config_tree_str(cmd, allocation_thin_pool_discards_CFG, profile))) {
			log_error(INTERNAL_ERROR "Could not find configuration.");
			return 0;
		}
		if (!set_pool_discards(discards, str))
			return_0;
	}

	if (!(passed_args & PASS_ARG_ZERO))
		*zero = find_config_tree_bool(cmd, allocation_thin_pool_zero_CFG, profile);

	if (!(attr & THIN_FEATURE_BLOCK_SIZE) &&
	    (*chunk_size & (*chunk_size - 1))) {
		log_error("Chunk size must be a power of 2 for this thin target version.");
		return 0;
	}

	if (!pool_metadata_size) {
		/* Defaults to nr_pool_blocks * 64b converted to size in sectors */
		pool_metadata_size = (uint64_t) pool_data_extents * extent_size /
			(*chunk_size * (SECTOR_SIZE / UINT64_C(64)));
		/* Check if we could eventually use bigger chunk size */
		if (!(passed_args & PASS_ARG_CHUNK_SIZE)) {
			while ((pool_metadata_size >
				(DEFAULT_THIN_POOL_OPTIMAL_SIZE / SECTOR_SIZE)) &&
			       (*chunk_size < DM_THIN_MAX_DATA_BLOCK_SIZE)) {
				*chunk_size <<= 1;
				pool_metadata_size >>= 1;
			}
			log_verbose("Setting chunk size to %s.",
				    display_size(cmd, *chunk_size));
		} else if (pool_metadata_size > (DEFAULT_THIN_POOL_MAX_METADATA_SIZE * 2)) {
			/* Suggest bigger chunk size */
			estimate_chunk_size =
				_estimate_chunk_size((uint64_t) pool_data_extents * extent_size,
						     (DEFAULT_THIN_POOL_MAX_METADATA_SIZE * 2), attr);
			log_warn("WARNING: Chunk size is too small for pool, suggested minimum is %s.",
				 display_size(cmd, estimate_chunk_size));
		}

		/* Round up to extent size silently */
		if (pool_metadata_size % extent_size)
			pool_metadata_size += extent_size - pool_metadata_size % extent_size;
	} else {
		estimate_chunk_size =
			_estimate_chunk_size((uint64_t) pool_data_extents * extent_size,
					     pool_metadata_size, attr);
		if (estimate_chunk_size < DM_THIN_MIN_DATA_BLOCK_SIZE)
			estimate_chunk_size = DM_THIN_MIN_DATA_BLOCK_SIZE;
		else if (estimate_chunk_size > DM_THIN_MAX_DATA_BLOCK_SIZE)
			estimate_chunk_size = DM_THIN_MAX_DATA_BLOCK_SIZE;

		/* Check to eventually use bigger chunk size */
		if (!(passed_args & PASS_ARG_CHUNK_SIZE)) {
			*chunk_size = estimate_chunk_size;
			log_verbose("Setting chunk size %s.", display_size(cmd, *chunk_size));
		} else if (*chunk_size < estimate_chunk_size) {
			/* Suggest bigger chunk size */
			log_warn("WARNING: Chunk size is smaller then suggested minimum size %s.",
				 display_size(cmd, estimate_chunk_size));
		}
	}

	if (pool_metadata_size > (2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE)) {
		pool_metadata_size = 2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE;
		if (passed_args & PASS_ARG_POOL_METADATA_SIZE)
			log_warn("WARNING: Maximum supported pool metadata size is %s.",
				 display_size(cmd, pool_metadata_size));
	} else if (pool_metadata_size < (2 * DEFAULT_THIN_POOL_MIN_METADATA_SIZE)) {
		pool_metadata_size = 2 * DEFAULT_THIN_POOL_MIN_METADATA_SIZE;
		if (passed_args & PASS_ARG_POOL_METADATA_SIZE)
			log_warn("WARNING: Minimum supported pool metadata size is %s.",
				 display_size(cmd, pool_metadata_size));
	}

	if (!(*pool_metadata_extents =
	      extents_from_size(vg->cmd, pool_metadata_size, extent_size)))
		return_0;

	return 1;
}

int set_pool_discards(thin_discards_t *discards, const char *str)
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

int lv_is_thin_origin(const struct logical_volume *lv, unsigned int *snap_count)
{
	struct seg_list *segl;
	int r = 0;

	if (snap_count)
		*snap_count = 0;

	if (!lv_is_thin_volume(lv) ||
	    dm_list_empty(&lv->segs_using_this_lv))
		return 0;

	dm_list_iterate_items(segl, &lv->segs_using_this_lv) {
		if (segl->seg->origin == lv) {
			r = 1;
			if (snap_count)
				(*snap_count)++;
			else
				/* not interested in number of snapshots */
				break;
		}
	}

	return r;
}

/*
 * Explict check of new thin pool for usability
 *
 * Allow use of thin pools by external apps. When lvm2 metadata has
 * transaction_id == 0 for a new thin pool, it will explicitely validate
 * the pool is still unused.
 *
 * To prevent lvm2 to create thin volumes in externally used thin pools
 * simply increment its transaction_id.
 */
int check_new_thin_pool(const struct logical_volume *pool_lv)
{
	struct cmd_context *cmd = pool_lv->vg->cmd;
	uint64_t transaction_id;

	/* For transaction_id check LOCAL activation is required */
	if (!activate_lv_excl_local(cmd, pool_lv)) {
		log_error("Aborting. Failed to locally activate thin pool %s.",
			  display_lvname(pool_lv));
		return 0;
	}

	/* With volume lists, check pool really is locally active */
	if (!lv_thin_pool_transaction_id(pool_lv, &transaction_id)) {
		log_error("Cannot read thin pool %s transaction id locally, perhaps skipped in lvm.conf volume_list?",
			  display_lvname(pool_lv));
		return 0;
	}

	/* Require pool to have same transaction_id as new  */
	if (first_seg(pool_lv)->transaction_id != transaction_id) {
		log_error("Cannot use thin pool %s with transaction id "
			  "%" PRIu64 " for thin volumes. "
			  "Expected transaction id %" PRIu64 ".",
			  display_lvname(pool_lv), transaction_id,
			  first_seg(pool_lv)->transaction_id);
		return 0;
	}

	log_verbose("Deactivating public thin pool %s",
		    display_lvname(pool_lv));

	/* Prevent any 'race' with in-use thin pool and always deactivate */
	if (!deactivate_lv(pool_lv->vg->cmd, pool_lv)) {
		log_error("Aborting. Could not deactivate thin pool %s.",
			  display_lvname(pool_lv));
		return 0;
	}

	return 1;
}
