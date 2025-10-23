/*
 * Copyright (C) 2014-2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/metadata/metadata.h"
#include "lib/locking/locking.h"
#include "lib/misc/lvm-string.h"
#include "lib/commands/toolcontext.h"
#include "lib/display/display.h"
#include "lib/metadata/segtype.h"
#include "lib/activate/activate.h"
#include "lib/config/defaults.h"
#include "lib/metadata/lv_alloc.h"
#include "lib/misc/lvm-signal.h"

/* https://github.com/jthornber/thin-provisioning-tools/blob/master/caching/cache_metadata_size.cc */
#define DM_TRANSACTION_OVERHEAD		4096  /* KiB */
#define DM_BYTES_PER_BLOCK		16 /* bytes */
#define DM_HINT_OVERHEAD_PER_BLOCK	8  /* bytes */
#define DM_MAX_HINT_WIDTH		(4+16)  /* bytes.  FIXME Configurable? */

const char *cache_mode_num_to_str(cache_mode_t mode)
{
	switch (mode) {
	case CACHE_MODE_WRITETHROUGH:
		return "writethrough";
	case CACHE_MODE_WRITEBACK:
		return "writeback";
	case CACHE_MODE_PASSTHROUGH:
		return "passthrough";
	default:
		return NULL;
	}
}

const char *get_cache_mode_name(const struct lv_segment *pool_seg)
{
	const char *str;
		
	if (!(str = cache_mode_num_to_str(pool_seg->cache_mode))) {
		log_error(INTERNAL_ERROR "Cache pool %s has undefined cache mode, using writethrough instead.",
			  display_lvname(pool_seg->lv));
		str = "writethrough";
	}
	return str;
}

const char *display_cache_mode(const struct lv_segment *seg)
{
	const struct lv_segment *setting_seg = NULL;

	if (seg_is_cache(seg) && lv_is_cache_vol(seg->pool_lv))
		setting_seg = seg;

	else if (seg_is_cache_pool(seg))
		setting_seg = seg;

	else if (seg_is_cache(seg))
		setting_seg = first_seg(seg->pool_lv);

	if (!setting_seg || (setting_seg->cache_mode == CACHE_MODE_UNSELECTED))
		return "";

	return cache_mode_num_to_str(setting_seg->cache_mode);
}

int set_cache_mode(cache_mode_t *mode, const char *cache_mode)
{
	if (!strcasecmp(cache_mode, "writethrough"))
		*mode = CACHE_MODE_WRITETHROUGH;
	else if (!strcasecmp(cache_mode, "writeback"))
		*mode = CACHE_MODE_WRITEBACK;
	else if (!strcasecmp(cache_mode, "passthrough"))
		*mode = CACHE_MODE_PASSTHROUGH;
	else {
		log_error("Unknown cache mode: %s.", cache_mode);
		return 0;
	}

	return 1;
}

static cache_mode_t _get_cache_mode_from_config(struct cmd_context *cmd,
						struct profile *profile,
						struct logical_volume *lv)
{
	cache_mode_t mode;
	const char *str;
	int id;

	/* Figure default settings from config/profiles */
	id = allocation_cache_mode_CFG;

	/* If present, check backward compatible settings */
	if (!find_config_node(cmd, cmd->cft, id) &&
	    find_config_node(cmd, cmd->cft, allocation_cache_pool_cachemode_CFG))
		id = allocation_cache_pool_cachemode_CFG;

	if (!(str = find_config_tree_str(cmd, id, profile))) {
		log_error(INTERNAL_ERROR "Cache mode is not determined.");
		return CACHE_MODE_WRITETHROUGH;
	}

	if (!(set_cache_mode(&mode, str)))
		return CACHE_MODE_WRITETHROUGH;

	return mode;
}

int cache_set_cache_mode(struct lv_segment *seg, cache_mode_t mode)
{
	struct cmd_context *cmd = seg->lv->vg->cmd;
	struct lv_segment *setting_seg;

	/*
	 * Don't set a cache mode on an unused cache pool, the
	 * cache mode will be set when it's attached.
	 */
	if (seg_is_cache_pool(seg) && (mode == CACHE_MODE_UNSELECTED))
		return 1;

	if (seg_is_cache(seg) && lv_is_cache_vol(seg->pool_lv))
		setting_seg = seg;

	else if (seg_is_cache_pool(seg))
		setting_seg = seg;

	else if (seg_is_cache(seg))
		setting_seg = first_seg(seg->pool_lv);

	else {
		log_error(INTERNAL_ERROR "Cannot set cache mode for non cache volume %s.",
			  display_lvname(seg->lv));
		return 0;
	}

	if (mode != CACHE_MODE_UNSELECTED) {
		setting_seg->cache_mode = mode;
		return 1;
	}

	if (setting_seg->cache_mode != CACHE_MODE_UNSELECTED)
		return 1;

	setting_seg->cache_mode = _get_cache_mode_from_config(cmd, seg->lv->profile, seg->lv);

	return 1;
}

/*
 * At least warn a user if certain cache stacks may present some problems
 */
void cache_check_for_warns(const struct lv_segment *seg)
{
	struct logical_volume *origin_lv = seg_lv(seg, 0);

	if (lv_is_raid(origin_lv) &&
	    first_seg(seg->pool_lv)->cache_mode == CACHE_MODE_WRITEBACK)
		log_warn("WARNING: Data redundancy could be lost with writeback "
			 "caching of raid logical volume!");
}

/*
 * Returns the minimum size of cache metadata volume for given cache data size and
 * and cache chunk size (all in/out values in sectors)
 * Default metadata size is: (Overhead + mapping size + hint size)
 */
static uint64_t _cache_min_metadata_size(uint64_t data_size, uint32_t chunk_size)
{
	/* Used space for mapping and hints for each cached chunk in bytes
	 * (matching thin-tools cache_metadata_size.cc) */
	const uint64_t chunk_overhead = (DM_BYTES_PER_BLOCK + DM_MAX_HINT_WIDTH + DM_HINT_OVERHEAD_PER_BLOCK);
	const uint64_t transaction_overhead = DM_TRANSACTION_OVERHEAD * 1024; /* 4MiB */

	/* Number of cache chunks we have in caching volume */
	uint64_t nr_chunks = data_size / chunk_size;
	/* Minimal size of metadata volume converted back to sectors */
	uint64_t min_meta_size = (transaction_overhead + nr_chunks * chunk_overhead +
				  (SECTOR_SIZE - 1)) >> SECTOR_SHIFT;

	return min_meta_size;
}

int update_cache_pool_params(struct cmd_context *cmd,
			     struct profile *profile,
			     uint32_t extent_size,
			     const struct segment_type *segtype,
			     unsigned attr,
			     uint32_t pool_data_extents,
			     uint32_t *pool_metadata_extents,
			     struct logical_volume *metadata_lv,
			     unsigned *chunk_size_calc_policy, uint32_t *chunk_size)
{
	uint64_t min_meta_size;
	uint64_t pool_metadata_size = (uint64_t) *pool_metadata_extents * extent_size;
	uint64_t pool_data_size = (uint64_t) pool_data_extents * extent_size;
	const uint64_t max_chunks =
		get_default_allocation_cache_pool_max_chunks_CFG(cmd, profile);
	/* min chunk size in a multiple of DM_CACHE_MIN_DATA_BLOCK_SIZE */
	uint64_t min_chunk_size = (((pool_data_size + max_chunks - 1) / max_chunks +
				    DM_CACHE_MIN_DATA_BLOCK_SIZE - 1) /
				   DM_CACHE_MIN_DATA_BLOCK_SIZE) * DM_CACHE_MIN_DATA_BLOCK_SIZE;

	*chunk_size_calc_policy = CHUNK_SIZE_CALC_POLICY_UNSELECTED;

	if (!*chunk_size) {
		if (!(*chunk_size = find_config_tree_int(cmd, allocation_cache_pool_chunk_size_CFG,
							 profile) * 2)) {
			*chunk_size = get_default_allocation_cache_pool_chunk_size_CFG(cmd,
										       profile);
			/* Use power-of-2 for min chunk size when unspecified */
			min_chunk_size = UINT64_C(1) << (32 - clz(min_chunk_size - 1));
		}
		if (*chunk_size < min_chunk_size) {
			/*
			 * When using more than 'standard' default,
			 * keep user informed he might be using things in unintended direction
			 */
			log_print_unless_silent("Using %s chunk size instead of default %s, "
						"so cache pool has less than " FMTu64 " chunks.",
						display_size(cmd, min_chunk_size),
						display_size(cmd, *chunk_size),
						max_chunks);
			*chunk_size = min_chunk_size;
		} else
			log_verbose("Setting chunk size to %s.",
				    display_size(cmd, *chunk_size));
	} else if (*chunk_size < min_chunk_size) {
		log_error("Chunk size %s is less than required minimal chunk size %s "
			  "for a cache pool of %s size and limit " FMTu64 " chunks.",
			  display_size(cmd, *chunk_size),
			  display_size(cmd, min_chunk_size),
			  display_size(cmd, pool_data_size),
			  max_chunks);
		log_error("To allow use of more chunks, see setting allocation/cache_pool_max_chunks.");
		return 0;
	}

	if (!validate_cache_chunk_size(cmd, *chunk_size))
		return_0;

	if ((uint64_t) *chunk_size > (uint64_t) pool_data_extents * extent_size) {
		log_error("Size of %s data volume cannot be smaller than chunk size %s.",
			  segtype->name, display_size(cmd, *chunk_size));
		return 0;
	}

	min_meta_size = _cache_min_metadata_size((uint64_t) pool_data_extents * extent_size, *chunk_size);
	min_meta_size = dm_round_up(min_meta_size, extent_size);

	if (!pool_metadata_size)
		pool_metadata_size = min_meta_size;

	if (!update_pool_metadata_min_max(cmd, extent_size,
					  min_meta_size,
					  (2 * DEFAULT_CACHE_POOL_MAX_METADATA_SIZE),
					  &pool_metadata_size,
					  metadata_lv,
					  pool_metadata_extents))
		return_0;

	log_verbose("Preferred pool metadata size %s.",
		    display_size(cmd, (uint64_t)*pool_metadata_extents * extent_size));

	return 1;
}

/*
 * Validate if existing cache-pool can be used with given chunk size
 * i.e. cache-pool metadata size fits all info.
 */
int validate_lv_cache_chunk_size(struct logical_volume *pool_lv, uint32_t chunk_size)
{
	struct volume_group *vg = pool_lv->vg;
	const uint64_t max_chunks = get_default_allocation_cache_pool_max_chunks_CFG(vg->cmd, pool_lv->profile);
	uint64_t min_size = _cache_min_metadata_size(pool_lv->size, chunk_size);
	uint64_t chunks = pool_lv->size / chunk_size;
	int r = 1;

	if (min_size > first_seg(pool_lv)->metadata_lv->size) {
		log_error("Cannot use chunk size %s with cache pool %s metadata size %s.",
			  display_size(vg->cmd, chunk_size),
			  display_lvname(pool_lv),
			  display_size(vg->cmd, first_seg(pool_lv)->metadata_lv->size));
		log_error("Minimal size for cache pool %s metadata with chunk size %s would be %s.",
			  display_lvname(pool_lv),
			  display_size(vg->cmd, chunk_size),
			  display_size(vg->cmd, min_size));
		r = 0;
	}

	if (chunks > max_chunks) {
		log_error("Cannot use too small chunk size %s with cache pool %s data volume size %s.",
			  display_size(vg->cmd, chunk_size),
			  display_lvname(pool_lv),
			  display_size(pool_lv->vg->cmd, pool_lv->size));
		log_error("Maximum configured chunks for a cache pool is " FMTu64 ".",
			  max_chunks);
		log_error("Use smaller cache pool (<%s) or bigger cache chunk size (>=%s) or enable higher "
			  "values in 'allocation/cache_pool_max_chunks'.",
			  display_size(vg->cmd, chunk_size * max_chunks),
			  display_size(vg->cmd, pool_lv->size / max_chunks));
		r = 0;
	}

	return r;
}
/*
 * Validate arguments for converting origin into cached volume with given cache pool.
 *
 * Always validates origin_lv, and when it is known also cache pool_lv
 */
int validate_lv_cache_create_pool(const struct logical_volume *pool_lv)
{
	struct lv_segment *seg;

	if (lv_is_locked(pool_lv)) {
		log_error("Cannot use locked cache pool %s.",
			  display_lvname(pool_lv));
		return 0;
	}

	if (!dm_list_empty(&pool_lv->segs_using_this_lv)) {
		seg = get_only_segment_using_this_lv(pool_lv);
		log_error("Logical volume %s is already in use by %s.",
			  display_lvname(pool_lv),
			  seg ? display_lvname(seg->lv) : "another LV");
		return 0;
	}

	return 1;
}

int validate_lv_cache_create_origin(const struct logical_volume *origin_lv)
{
	if (lv_is_locked(origin_lv)) {
		log_error("Cannot use locked origin volume %s.",
			  display_lvname(origin_lv));
		return 0;
	}

	/* For now we only support conversion of thin pool data volume */
	if (!lv_is_visible(origin_lv) &&
	    !lv_is_thin_pool_data(origin_lv) &&
	    !lv_is_vdo_pool_data(origin_lv)) {
		log_error("Can't convert internal LV %s.", display_lvname(origin_lv));
		return 0;
	}

	if (lv_is_cache_type(origin_lv) ||
	    lv_is_mirror_type(origin_lv) ||
	    lv_is_merging_origin(origin_lv) ||
	    lv_is_cow(origin_lv) || lv_is_merging_cow(origin_lv)) {
		log_error("Cache is not supported with %s segment type of the original logical volume %s.",
			  lvseg_name(first_seg(origin_lv)), display_lvname(origin_lv));
		return 0;
	}

	return 1;
}

int validate_cache_chunk_size(struct cmd_context *cmd, uint32_t chunk_size)
{
	const uint32_t min_size = DM_CACHE_MIN_DATA_BLOCK_SIZE;
	const uint32_t max_size = DM_CACHE_MAX_DATA_BLOCK_SIZE;
	int r = 1;

	if ((chunk_size < min_size) || (chunk_size > max_size)) {
		log_error("Cache chunk size %s is not in the range %s to %s.",
			  display_size(cmd, chunk_size),
			  display_size(cmd, min_size),
			  display_size(cmd, max_size));
		r = 0;
	}

	if (chunk_size & (min_size - 1)) {
		log_error("Cache chunk size %s must be a multiple of %s.",
			  display_size(cmd, chunk_size),
			  display_size(cmd, min_size));
		r = 0;
	}

	return r;
}

/*
 * lv_cache_create
 * @pool
 * @origin
 *
 * Given a cache_pool and an origin, link the two and create a
 * cached LV.
 *
 * Returns: cache LV on success, NULL on failure
 */
struct logical_volume *lv_cache_create(struct logical_volume *pool_lv,
				       struct logical_volume *origin_lv)
{
	char cpool_name[NAME_LEN];
	const struct segment_type *segtype;
	struct cmd_context *cmd = pool_lv->vg->cmd;
	struct logical_volume *cache_lv = origin_lv;
	struct lv_segment *seg;

	if (!validate_lv_cache_create_pool(pool_lv) ||
	    !validate_lv_cache_create_origin(cache_lv))
		return_NULL;

	if (lv_is_thin_pool(cache_lv) || lv_is_vdo_pool(cache_lv))
		cache_lv = seg_lv(first_seg(cache_lv), 0); /* cache _tdata */

	if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_CACHE)))
		return_NULL;
	/* coverity[format_string_injection] lv name is already validated */
	if (!insert_layer_for_lv(cmd, cache_lv, 0, "_corig"))
		return_NULL;

	seg = first_seg(cache_lv);
	seg->segtype = segtype;

	if (!attach_pool_lv(seg, pool_lv, NULL, NULL, NULL))
		return_NULL;

	if (lv_is_cache_pool(pool_lv)) {
		/* Used cache-pool gets  _cpool suffix (easy to recognize from _cvol usage) */
		if (dm_snprintf(cpool_name, sizeof(cpool_name), "%s_cpool", pool_lv->name) < 0) {
			log_error("Can't prepare new cachepool name for %s.", display_lvname(pool_lv));
			return NULL;
		}

		if (!lv_rename_update(cmd, pool_lv, cpool_name, 0))
			return_NULL;
	}

	if (!seg->lv->profile) /* Inherit profile from cache-pool */
		seg->lv->profile = seg->pool_lv->profile;

	return cache_lv;
}

/*
 * Checks cache status and loops until there are not dirty blocks
 * Set 1 to *is_clean when there are no dirty blocks on return.
 */
int lv_cache_wait_for_clean(struct logical_volume *cache_lv, int *is_clean)
{
	const struct logical_volume *lock_lv = lv_lock_holder(cache_lv);
	struct lv_segment *cache_seg = first_seg(cache_lv);
	struct lv_status_cache *status;
	int cleaner_policy = 0, writeback;
	uint64_t dirty_blocks;

	*is_clean = 0;

	//FIXME: use polling to do this...
	for (;;) {
		if (cleaner_policy && interruptible_usleep(500000)) {
			log_error("Flushing of %s aborted.", display_lvname(cache_lv));
			if (cache_seg->cleaner_policy) {
				cache_seg->cleaner_policy = 0;
				/* Restore normal table */
				sigint_clear();
				if (!lv_update_and_reload_origin(cache_lv))
					stack;
			}
			return 0;
		}

		if (!lv_cache_status(cache_lv, &status))
			return_0;

		if (status->cache->fail) {
			dm_pool_destroy(status->mem);
			log_warn("WARNING: Skipping flush for failed cache %s.",
				 display_lvname(cache_lv));
			return 1;
		}

		cleaner_policy = !strcmp(status->cache->policy_name, "cleaner");
		dirty_blocks = status->cache->dirty_blocks;
		writeback = (status->cache->feature_flags & DM_CACHE_FEATURE_WRITEBACK);
		dm_pool_destroy(status->mem);

		/* Only clear when policy is Clear or mode != writeback */
		if (!dirty_blocks && (cleaner_policy || !writeback))
			break;

		log_print_unless_silent("Flushing " FMTu64 " blocks for cache %s.",
					dirty_blocks, display_lvname(cache_lv));

		if (cleaner_policy)
			continue;

		if (!(cache_lv->status & LVM_WRITE)) {
			log_warn("WARNING: Dirty blocks found on read-only cache volume %s.",
				 display_lvname(cache_lv));
			/* TODO: can we actually clean something? */
		}

		/* Switch to cleaner policy to flush the cache */
		cache_seg->cleaner_policy = 1;
		/* Reload cache volume with "cleaner" policy */
		if (!lv_update_and_reload_origin(cache_lv))
			return_0;

		if (!sync_local_dev_names(cache_lv->vg->cmd)) {
			log_error("Failed to sync local devices when clearing cache volume %s.",
				  display_lvname(cache_lv));
			return 0;
		}
	}

	/*
	 * TODO: add check if extra suspend resume is necessary
	 * ATM this is workaround for missing cache sync when cache gets clean
	 */
	if (cleaner_policy) {
		if (!lv_refresh_suspend_resume(lock_lv))
			return_0;

		if (!sync_local_dev_names(cache_lv->vg->cmd)) {
			log_error("Failed to sync local devices after final clearing of cache %s.",
				  display_lvname(cache_lv));
			return 0;
		}
	}

	cache_seg->cleaner_policy = 0;
	*is_clean = 1;

	return 1;
}

/*
 * lv_cache_remove
 * @cache_lv
 *
 * Given a cache LV, remove the cache layer.  This will unlink
 * the origin and cache_pool/cachevol, remove the cache LV layer, and promote
 * the origin to a usable non-cached LV of the same name as the
 * given cache_lv.
 *
 * Returns: 1 on success, 0 on failure
 */
int lv_cache_remove(struct logical_volume *cache_lv)
{
	struct lv_segment *cache_seg = first_seg(cache_lv);
	struct logical_volume *corigin_lv;
	struct logical_volume *cache_pool_lv;
	struct id *data_id, *metadata_id;
	uint64_t data_len, metadata_len;
	cache_mode_t cache_mode;
	int temp_activated = 0;
	int is_clear;

	if (!lv_is_cache(cache_lv)) {
		log_error(INTERNAL_ERROR "LV %s is not cache volume.",
			  display_lvname(cache_lv));
		return 0;
	}

	if (lv_is_pending_delete(cache_lv)) {
		log_debug(INTERNAL_ERROR "LV %s is already dropped cache volume.",
			  display_lvname(cache_lv));
		goto remove;  /* Already dropped */
	}

	if (!lv_info(cache_lv->vg->cmd, cache_lv, 1, NULL, 0, 0)) {

		/*
		 * LV is inactive.  When used in writeback, it will
		 * need to be activated to write the cache content
		 * back to the main LV before detaching it.
		 */

		cache_mode = (lv_is_cache_pool(cache_seg->pool_lv)) ?
			first_seg(cache_seg->pool_lv)->cache_mode : cache_seg->cache_mode;
		switch (cache_mode) {
		case CACHE_MODE_WRITETHROUGH:
		case CACHE_MODE_PASSTHROUGH:
			/* For inactive pass/writethrough just drop cache layer */
			corigin_lv = seg_lv(cache_seg, 0);
			if (!detach_pool_lv(cache_seg))
				return_0;
			if (!remove_layer_from_lv(cache_lv, corigin_lv))
				return_0;
			if (!lv_remove(corigin_lv))
				return_0;
			return 1;
		default:
			cache_lv->status |= LV_TEMPORARY;
			if (!activate_lv(cache_lv->vg->cmd, cache_lv) ||
			    !lv_is_active(cache_lv)) {
				log_error("Failed to activate %s to flush cache.", display_lvname(cache_lv));
				return 0;
			}
			cache_lv->status &= ~LV_TEMPORARY;
			temp_activated = 1;
		}
	}

	/*
	 * FIXME:
	 * Before the link can be broken, we must ensure that the
	 * cache has been flushed.  This may already be the case
	 * if the cache mode is writethrough (or the cleaner
	 * policy is in place from a previous half-finished attempt
	 * to remove the cache_pool).  It could take a long time to
	 * flush the cache - it should probably be done in the background.
	 *
	 * Also, if we do perform the flush in the background and we
	 * happen to also be removing the cache/origin LV, then we
	 * could check if the cleaner policy is in place and simply
	 * remove the cache_pool then without waiting for the flush to
	 * complete.
	 */
	if (!lv_cache_wait_for_clean(cache_lv, &is_clear)) {
		if (temp_activated && !deactivate_lv(cache_lv->vg->cmd, cache_lv))
			stack;
		return_0;
	}

	if (temp_activated && !deactivate_lv(cache_lv->vg->cmd, cache_lv))
		log_warn("Failed to deactivate after cleaning cache.");

	cache_pool_lv = cache_seg->pool_lv;
	if (!detach_pool_lv(cache_seg))
		return_0;

	/*
	 * Drop layer from cache LV and make _corigin to appear again as regular LV
	 * And use 'existing' _corigin  volume to keep reference on cache-pool
	 * This way we still have a way to reference _corigin in dm table and we
	 * know it's been 'cache' LV  and we can drop all needed table entries via
	 * activation and deactivation of it.
	 *
	 * This 'cache' LV without origin is temporary LV, which still could be
	 * easily operated by lvm2 commands - it could be activate/deactivated/removed.
	 * However in the dm-table it will use 'error' target for _corigin volume.
	 */
	corigin_lv = seg_lv(cache_seg, 0);
	lv_set_visible(corigin_lv);

	if (!remove_layer_from_lv(cache_lv, corigin_lv))
		return_0;

	/* Preserve currently important data from original cache segment.
	 * TODO: can it be done without this ? */
	data_id = cache_seg->data_id;
	data_len = cache_seg->data_len;
	metadata_id = cache_seg->metadata_id;
	metadata_len = cache_seg->metadata_len;

	/* Replace 'error' with 'cache' segtype */
	cache_seg = first_seg(corigin_lv);
	if (!(cache_seg->segtype = get_segtype_from_string(corigin_lv->vg->cmd, SEG_TYPE_NAME_CACHE)))
		return_0;

	if (!add_lv_segment_areas(cache_seg, 1))
		return_0;

	if (!set_lv_segment_area_lv(cache_seg, 0, cache_lv, 0, 0))
		return_0;

	corigin_lv->le_count = cache_lv->le_count;
	corigin_lv->size = cache_lv->size;
	corigin_lv->status |= LV_PENDING_DELETE;

	/* Restore preserved data into a new cache segment that is going to be removed. */
	if ((cache_seg->data_len = data_len)) {
		cache_seg->metadata_len = metadata_len;
		cache_seg->data_id = data_id;
		cache_seg->metadata_id = metadata_id;
		cache_pool_lv->status |= LV_CACHE_VOL;
		/* Unused settings set only for passing metadata validation. */
		cache_seg->cache_mode = CACHE_MODE_WRITETHROUGH;
		cache_seg->chunk_size = DM_CACHE_MAX_DATA_BLOCK_SIZE;
		cache_seg->cache_metadata_format = CACHE_METADATA_FORMAT_2;
	}

	/* Reattach cache pool */
	if (!attach_pool_lv(cache_seg, cache_pool_lv, NULL, NULL, NULL))
		return_0;

	/* Suspend/resume also deactivates deleted LV via support of LV_PENDING_DELETE */
	if (!lv_update_and_reload(cache_lv))
		return_0;
	cache_lv = corigin_lv;
remove:
	if (!detach_pool_lv(cache_seg))
		return_0;

	if (!lv_remove(cache_lv)) /* Will use LV_PENDING_DELETE */
		return_0;

	/* CachePool or CacheVol is left inactive for further manipulation */

	return 1;
}

int lv_is_cache_origin(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	/* Make sure there's exactly one segment in segs_using_this_lv! */
	if (dm_list_empty(&lv->segs_using_this_lv) ||
	    (dm_list_size(&lv->segs_using_this_lv) > 1))
		return 0;

	seg = get_only_segment_using_this_lv(lv);
	return seg && lv_is_cache(seg->lv) && !lv_is_pending_delete(seg->lv) && (seg_lv(seg, 0) == lv);
}

static const char *_get_default_cache_policy(struct cmd_context *cmd)
{
	const struct segment_type *segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_CACHE);
	unsigned attr = ~0;
        const char *def = NULL;

	if (!segtype ||
	    !segtype->ops->target_present ||
	    !segtype->ops->target_present(cmd, NULL, &attr)) {
		log_warn("WARNING: Cannot detect default cache policy, using \""
			 DEFAULT_CACHE_POLICY "\".");
		return DEFAULT_CACHE_POLICY;
	}

	if (attr & CACHE_FEATURE_POLICY_SMQ)
		def = "smq";
	else if (attr & CACHE_FEATURE_POLICY_MQ)
		def = "mq";
	else {
		log_error("Default cache policy is not available.");
		return NULL;
	}

	log_debug_metadata("Detected default cache_policy \"%s\".", def);

	return def;
}

/* Autodetect best available cache metadata format for a user */
static cache_metadata_format_t _get_default_cache_metadata_format(struct cmd_context *cmd)
{
	const struct segment_type *segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_CACHE);
	unsigned attr = 0;
	cache_metadata_format_t f;

	if (!segtype ||
	    !segtype->ops->target_present ||
	    !segtype->ops->target_present(cmd, NULL, &attr)) {
		f = CACHE_METADATA_FORMAT_1;
		log_warn("WARNING: Cannot detect default cache metadata format, using format: %u.", f);
	} else {
		f = (attr & CACHE_FEATURE_METADATA2) ? CACHE_METADATA_FORMAT_2 : CACHE_METADATA_FORMAT_1;
		log_debug_metadata("Detected default cache metadata format: %u.", f);
	}

	return f;
}

int cache_set_policy(struct lv_segment *lvseg, const char *name,
		     const struct dm_config_tree *settings)
{
	struct lv_segment *seg;
	struct dm_config_node *cn;
	const struct dm_config_node *cns;
	struct dm_config_tree *old = NULL, *new = NULL, *tmp = NULL;
	int r = 0;
	struct profile *profile = lvseg->lv->profile;

	if (seg_is_cache_pool(lvseg)) {
		if (!name && !settings)
			return 1; /* Policy and settings can be selected later when caching LV */
	}

	if (seg_is_cache(lvseg) && lv_is_cache_vol(lvseg->pool_lv))
		seg = lvseg;

	else if (seg_is_cache_pool(lvseg))
		seg = lvseg;

	else if (seg_is_cache(lvseg))
		seg = first_seg(lvseg->pool_lv);

	else {
		log_error(INTERNAL_ERROR "Cannot set cache metadata format for non cache volume %s.",
			  display_lvname(lvseg->lv));
		return 0;
	}

	if (name) {
		if (!(seg->policy_name = dm_pool_strdup(seg->lv->vg->vgmem, name))) {
			log_error("Failed to duplicate policy name.");
			return 0;
		}
	} else if (!seg->policy_name) {
		if (!(seg->policy_name = find_config_tree_str(seg->lv->vg->cmd, allocation_cache_policy_CFG,
							      profile)) &&
		    !(seg->policy_name = _get_default_cache_policy(seg->lv->vg->cmd)))
			return_0;
		if (!seg->policy_name) {
			log_error(INTERNAL_ERROR "Can't set policy settings without policy name.");
			return 0;
		}
	}

	if (settings) {
		if (seg->policy_settings) {
			if (!(old = dm_config_create()))
				goto_out;
			if (!(new = dm_config_create()))
				goto_out;
			new->root = settings->root;
			old->root = seg->policy_settings;
			new->cascade = old;
			if (!(tmp = dm_config_flatten(new)))
				goto_out;
		}

		if ((cn = dm_config_find_node((tmp) ? tmp->root : settings->root, "policy_settings")) &&
		    !(seg->policy_settings = dm_config_clone_node_with_mem(seg->lv->vg->vgmem, cn, 0)))
			goto_out;
	} else if (!seg->policy_settings) {
		if ((cns = find_config_tree_node(seg->lv->vg->cmd, allocation_cache_settings_CFG_SECTION,
						 profile))) {
			/* Try to find our section for given policy */
			for (cn = cns->child; cn; cn = cn->sib) {
				if (!cn->child)
					continue; /* Ignore section without settings */

				if (cn->v || strcmp(cn->key, seg->policy_name) != 0)
					continue; /* Ignore mismatching sections */

				/* Clone nodes with policy name */
				if (!(seg->policy_settings = dm_config_clone_node_with_mem(seg->lv->vg->vgmem,
											   cn, 0)))
					return_0;

				/* Replace policy name key with 'policy_settings' */
				seg->policy_settings->key = "policy_settings";
				break; /* Only first match counts */
			}
		}
	}

restart: /* remove any 'default" nodes */
	cn = seg->policy_settings ? seg->policy_settings->child : NULL;
	while (cn) {
		if (cn->v->type == DM_CFG_STRING && !strcmp(cn->v->v.str, "default")) {
			dm_config_remove_node(seg->policy_settings, cn);
			goto restart;
		}
		cn = cn->sib;
	}

	r = 1;

out:
	if (tmp)
		dm_config_destroy(tmp);
	if (new)
		dm_config_destroy(new);
	if (old)
		dm_config_destroy(old);

	return r;
}

/*
 * Sets metadata format on cache pool segment with these rules:
 * 1. When 'cache-pool' segment is passed, sets only for selected formats (1 or 2).
 * 2. For 'cache' segment passed in we know cache pool segment.
 *      When passed format is 0 (UNSELECTED) with 'cache' segment - it's the moment
 *      lvm2 has to figure out 'default' metadata format (1 or 2) from
 *      configuration or profiles.
 * 3. If still unselected or selected format is != 1, figure the best supported format
 *    and either use it or validate users settings is possible.
 *
 * Reasoning: A user may create cache-pool and may or may not specify CMFormat.
 * If the CMFormat has been selected (1 or 2) store this in metadata, otherwise
 * for an unused cache-pool UNSELECTED CMFormat is used. When caching LV, CMFormat
 * must be decided and from this moment it's always stored. To support backward
 * compatibility 'CMFormat 1' is used when it is NOT specified for a cached LV in
 * lvm2 metadata (no metadata_format=#F element in cache-pool segment).
 */
int cache_set_metadata_format(struct lv_segment *seg, cache_metadata_format_t format)
{
	cache_metadata_format_t best;
	struct profile *profile = seg->lv->profile;

	if (seg_is_cache(seg))
		seg = first_seg(seg->pool_lv);
	else if (seg_is_cache_pool(seg)) {
		if (format == CACHE_METADATA_FORMAT_UNSELECTED)
			return 1; /* Format can be selected later when caching LV */
	} else {
		log_error(INTERNAL_ERROR "Cannot set cache metadata format for non cache volume %s.",
			  display_lvname(seg->lv));
		return 0;
	}

	/*
	 * If policy is unselected, but format 2 is selected, policy smq is enforced.
	 */
	if (!seg->policy_name) {
		if (format == CACHE_METADATA_FORMAT_2)
			seg->policy_name = "smq";
	}

	/* Check if we need to search for configured cache metadata format */
	if (format == CACHE_METADATA_FORMAT_UNSELECTED) {
		if (seg->cache_metadata_format != CACHE_METADATA_FORMAT_UNSELECTED)
			return 1; /* Format already selected in cache pool */

		/* Check configurations and profiles */
		switch (find_config_tree_int(seg->lv->vg->cmd,
					     allocation_cache_metadata_format_CFG,
					     profile)) {
		case 1:  format = CACHE_METADATA_FORMAT_1; break;
		case 2:  format = CACHE_METADATA_FORMAT_2; break;
		default: format = CACHE_METADATA_FORMAT_UNSELECTED; break;
		}
	}

	/* See what is a 'best' available cache metadata format
	 * when the specified format is other then always existing CMFormat 1 */
	if (format != CACHE_METADATA_FORMAT_1) {
		best = _get_default_cache_metadata_format(seg->lv->vg->cmd);

		/* Format was not selected, so use best present on a system */
		if (format == CACHE_METADATA_FORMAT_UNSELECTED)
			format = best;
		else if (format != best) {
			/* Format is not valid (Only Format 1 or 2 is supported ATM) */
			log_error("Cache metadata format %u is not supported by kernel target.", format);
			return 0;
		}
	}

	switch (format) {
	case CACHE_METADATA_FORMAT_2: seg->lv->status |= LV_METADATA_FORMAT; break;
	case CACHE_METADATA_FORMAT_1: seg->lv->status &= ~LV_METADATA_FORMAT; break;
	default:
		log_error(INTERNAL_ERROR "Invalid cache metadata format %u for cache volume %s.",
			  format, display_lvname(seg->lv));
		return 0;
	}

	seg->cache_metadata_format = format;

	return 1;
}

#define ONE_MB_IN_SECTORS 2048 /* 1MB in sectors */
#define ONE_GB_IN_SECTORS 2097152 /* 1GB in sectors */

int cache_vol_set_params(struct cmd_context *cmd,
		     struct logical_volume *cache_lv,
		     struct logical_volume *pool_lv,
		     uint64_t poolmetadatasize,
		     uint32_t chunk_size,
		     cache_metadata_format_t format,
		     cache_mode_t mode,
		     const char *policy,
		     const struct dm_config_tree *settings)
{
	struct dm_pool *mem = cache_lv->vg->vgmem;
	struct profile *profile = cache_lv->profile;
	struct lv_segment *cache_seg = first_seg(cache_lv);
	struct logical_volume *corig_lv = seg_lv(cache_seg, 0);
	const char *policy_name = NULL;
	struct dm_config_node *policy_settings = NULL;
	const struct dm_config_node *cns;
	struct dm_config_node *cn;
	uint64_t meta_size = 0;
	uint64_t data_size = 0;
	uint64_t max_chunks;
	uint32_t min_meta_size;
	uint32_t max_meta_size;
	uint32_t extent_size;

	/* all _size variables in units of sectors (512 bytes) */


	/*
	 * cache format: only create new cache LVs with 2.
	 */

	if (format == CACHE_METADATA_FORMAT_UNSELECTED)
		format = CACHE_METADATA_FORMAT_2;
	if (format == CACHE_METADATA_FORMAT_1) {
		log_error("Use cache metadata format 2.");
		return 0;
	}


	/*
	 * cache mode: get_cache_params() gets mode from --cachemode or sets
	 * UNSEL.  When unspecified, it comes from config.
	 */

	if (mode == CACHE_MODE_UNSELECTED)
		mode = _get_cache_mode_from_config(cmd, profile, cache_lv);

	cache_seg->cache_mode = mode;


	/*
	 * chunk size: get_cache_params() get chunk_size from --chunksize or
	 * sets 0.  When unspecified it comes from config or default.
	 *
	 * cache_pool_chunk_size in lvm.conf, DEFAULT_CACHE_POOL_CHUNK_SIZE,
	 * and DEFAULT_CACHE_POOL_MAX_METADATA_SIZE are in KiB, so *2 turn
	 * them into sectors.
	 */

	if (!chunk_size)
		chunk_size = find_config_tree_int(cmd, allocation_cache_pool_chunk_size_CFG, cache_lv->profile) * 2;

	if (!chunk_size)
		chunk_size = get_default_allocation_cache_pool_chunk_size_CFG(cmd, profile);

	if (!validate_cache_chunk_size(cmd, chunk_size))
		return_0;


	/*
	 * metadata size: can be specified with --poolmetadatasize,
	 * otherwise it's set according to the size of the cache.
	 * data size: the LV size minus the metadata size.
	 */

	if (!(extent_size = pool_lv->vg->extent_size)) {
		log_error(INTERNAL_ERROR "Extent size can't be 0.");
		return 0;
	}
	min_meta_size = extent_size;
	max_meta_size = 2 * DEFAULT_CACHE_POOL_MAX_METADATA_SIZE; /* 2x for KiB to sectors */

	if (pool_lv->size < (extent_size * 2)) {
		log_error("The minimum cache size is two extents (%s bytes).",
			  display_size(cmd, extent_size * 2));
		return 0;
	}

	if (poolmetadatasize) {
		meta_size = poolmetadatasize; /* in sectors, from --poolmetadatasize, see _size_arg() */

		if (meta_size > max_meta_size) {
			meta_size = max_meta_size;
			log_print_unless_silent("Rounding down metadata size to max size %s",
						display_size(cmd, meta_size));
		}
		if (meta_size < min_meta_size) {
			meta_size = min_meta_size;
			log_print_unless_silent("Rounding up metadata size to min size %s",
						display_size(cmd, meta_size));
		}

		if (meta_size % extent_size) {
			meta_size += extent_size - meta_size % extent_size;
			log_print_unless_silent("Rounding up metadata size to full physical extent %s",
						display_size(cmd, meta_size));
		}
	}

	if (!meta_size) {
		meta_size = _cache_min_metadata_size(pool_lv->size, chunk_size);

		/* fix bad value from _cache_min_metadata_size */
		if (meta_size > (pool_lv->size / 2))
			meta_size = pool_lv->size / 2;

		if (meta_size < min_meta_size)
			meta_size = min_meta_size;

		if (meta_size % extent_size)
			meta_size += extent_size - meta_size % extent_size;
	}

	data_size = pool_lv->size - meta_size;

	max_chunks = get_default_allocation_cache_pool_max_chunks_CFG(cmd, profile);

	if (data_size / chunk_size > max_chunks) {
		log_error("Cache data blocks %llu and chunk size %u exceed max chunks %llu.",
			  (unsigned long long)data_size, chunk_size, (unsigned long long)max_chunks);
		log_error("Use smaller cache, larger --chunksize or increase max chunks setting.");
		return 0;
	}


	/*
	 * cache policy: get_cache_params() gets policy from --cachepolicy,
	 * or sets NULL.
	 */

	if (!policy)
		policy = find_config_tree_str(cmd, allocation_cache_policy_CFG, profile);

	if (!policy)
		policy = _get_default_cache_policy(cmd);

	if (!policy) {
		log_error(INTERNAL_ERROR "Missing cache policy name.");
		return 0;
	}

	if (!(policy_name = dm_pool_strdup(mem, policy)))
		return_0;


	/*
	 * cache settings: get_cache_params() gets policy from --cachesettings,
	 * or sets NULL.
	 * FIXME: code for this is a mess, mostly copied from cache_set_policy
	 * which is even worse.
	 */

	if (settings) {
		if ((cn = dm_config_find_node(settings->root, "policy_settings"))) {
			if (!(policy_settings = dm_config_clone_node_with_mem(mem, cn, 0)))
				return_0;
		}
	} else {
		if ((cns = find_config_tree_node(cmd, allocation_cache_settings_CFG_SECTION, profile))) {
			/* Try to find our section for given policy */
			for (cn = cns->child; cn; cn = cn->sib) {
				if (!cn->child)
					continue; /* Ignore section without settings */

				if (cn->v || strcmp(cn->key, policy_name) != 0)
					continue; /* Ignore mismatching sections */

				/* Clone nodes with policy name */
				if (!(policy_settings = dm_config_clone_node_with_mem(mem, cn, 0)))
					return_0;

				/* Replace policy name key with 'policy_settings' */
				policy_settings->key = "policy_settings";
				break; /* Only first match counts */
			}
		}
	}
  restart: /* remove any 'default" nodes */
	cn = policy_settings ? policy_settings->child : NULL;
	while (cn) {
		if (cn->v->type == DM_CFG_STRING && !strcmp(cn->v->v.str, "default")) {
			dm_config_remove_node(policy_settings, cn);
			goto restart;
		}
		cn = cn->sib;
	}


	log_debug("Setting LV %s cache on %s meta start 0 len %llu data start %llu len %llu sectors",
		  display_lvname(cache_lv), display_lvname(pool_lv),
		  (unsigned long long)meta_size,
		  (unsigned long long)meta_size,
		  (unsigned long long)data_size);
	log_debug("Setting LV %s cache format %u policy %s chunk_size %u sectors",
		  display_lvname(cache_lv), format, policy_name, chunk_size);

	if (lv_is_raid(corig_lv) && (mode == CACHE_MODE_WRITEBACK))
		log_warn("WARNING: Data redundancy could be lost with writeback caching of raid logical volume!");

	if (lv_is_thin_pool_data(cache_lv)) {
		log_warn("WARNING: Thin pool data will not be automatically extended when cached.");
		log_warn("WARNING: Manual splitcache is required before extending thin pool data.");
	}

	cache_seg->chunk_size = chunk_size;
	cache_seg->metadata_start = 0;
	cache_seg->metadata_len = meta_size;
	cache_seg->data_start = meta_size;
	cache_seg->data_len = data_size;
	cache_seg->cache_metadata_format = format;
	cache_seg->policy_name = policy_name;
	cache_seg->policy_settings = policy_settings;
	/* Since we add -cdata  and -cmeta to UUID we use CacheVol LV UUID */
	cache_seg->data_id = cache_seg->metadata_id = NULL;

	return 1;
}

int cache_set_params(struct lv_segment *seg,
		     uint32_t chunk_size,
		     cache_metadata_format_t format,
		     cache_mode_t mode,
		     const char *policy_name,
		     const struct dm_config_tree *policy_settings)
{
	struct lv_segment *pool_seg;
	struct cmd_context *cmd = seg->lv->vg->cmd;

	if (!cache_set_cache_mode(seg, mode))
		return_0;

	if (!cache_set_policy(seg, policy_name, policy_settings))
		return_0;

	if (!cache_set_metadata_format(seg, format))
		return_0;

	pool_seg = seg_is_cache(seg) ? first_seg(seg->pool_lv) : seg;

	if (chunk_size) {
		if (seg_is_cache(seg) &&
		    !validate_lv_cache_chunk_size(pool_seg->lv, chunk_size))
			return_0;
		pool_seg->chunk_size = chunk_size;
	} else if (seg_is_cache(seg)) {
		/* Chunk size in profile has priority over cache-pool chunk size */
		if ((chunk_size = find_config_tree_int(cmd, allocation_cache_pool_chunk_size_CFG,
						       seg->lv->profile) * 2)) {
			if (!validate_lv_cache_chunk_size(pool_seg->lv, chunk_size))
				return_0;
			if (pool_seg->chunk_size != chunk_size)
				log_verbose("Replacing chunk size %s in cache pool %s with "
					    "chunk size %s from profile.",
					    display_size(cmd, pool_seg->chunk_size),
					    display_lvname(seg->lv),
					    display_size(cmd, chunk_size));
			pool_seg->chunk_size = chunk_size;
		}
	} else if (seg_is_cache_pool(seg)) {
		if (!pool_seg->chunk_size &&
		    /* TODO: some calc_policy solution for cache ? */
		    !recalculate_pool_chunk_size_with_dev_hints(pool_seg->lv,
								seg_lv(pool_seg, 0),
								CHUNK_SIZE_CALC_POLICY_GENERIC))
			return_0;
	}

	if (seg_is_cache(seg))
		cache_check_for_warns(seg);

	return 1;
}

/*
 * Wipe cache pool metadata area before use.
 *
 * Activates metadata volume as 'cache-pool' so regular wiping
 * of existing visible volume may proceed.
 */
int wipe_cache_pool(struct logical_volume *cache_pool_lv)
{
	int r;
	struct logical_volume *cache_data_lv;

	/* Only unused cache-pool could be activated and wiped */
	if (lv_is_used_cache_pool(cache_pool_lv) || lv_is_cache_vol(cache_pool_lv)) {
		log_error(INTERNAL_ERROR "Failed to wipe cache pool for volume %s.",
			  display_lvname(cache_pool_lv));
		return 0;
	}

	cache_data_lv = (lv_is_cache_pool(cache_pool_lv)) ?
		seg_lv(first_seg(cache_pool_lv), 0) : cache_pool_lv;

	if (cache_data_lv && seg_cannot_be_zeroed(first_seg(cache_data_lv))) {
		log_debug("Skipping wipe of %s volume with %s segtype.",
			  display_lvname(cache_data_lv),
			  first_seg(cache_data_lv)->segtype->name);
		return 1;
	}

	cache_pool_lv->status |= LV_TEMPORARY;
	if (!activate_lv(cache_pool_lv->vg->cmd, cache_pool_lv)) {
		log_error("Aborting. Failed to activate cache pool %s.",
			  display_lvname(cache_pool_lv));
		return 0;
	}
	cache_pool_lv->status &= ~LV_TEMPORARY;
	if (!(r = wipe_lv(cache_pool_lv, (struct wipe_params) { .do_zero = 1 }))) {
		log_error("Aborting. Failed to wipe cache pool %s.",
			  display_lvname(cache_pool_lv));
		/* Delay return of error after deactivation */
	}

	/* Deactivate cleared cache-pool metadata */
	if (!deactivate_lv(cache_pool_lv->vg->cmd, cache_pool_lv)) {
		log_error("Aborting. Could not deactivate cache pool %s.",
			  display_lvname(cache_pool_lv));
		r = 0;
	}

	return r;
}
