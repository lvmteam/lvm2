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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "metadata.h"
#include "locking.h"
#include "lvm-string.h"
#include "toolcontext.h"
#include "display.h"
#include "segtype.h"
#include "activate.h"
#include "defaults.h"
#include "lv_alloc.h"

/* https://github.com/jthornber/thin-provisioning-tools/blob/master/caching/cache_metadata_size.cc */
#define DM_TRANSACTION_OVERHEAD		4096  /* KiB */
#define DM_BYTES_PER_BLOCK		16 /* bytes */
#define DM_HINT_OVERHEAD_PER_BLOCK	8  /* bytes */
#define DM_MAX_HINT_WIDTH		(4+16)  /* bytes.  FIXME Configurable? */

int cache_mode_is_set(const struct lv_segment *seg)
{
	if (seg_is_cache(seg))
		seg = first_seg(seg->pool_lv);

	return (seg->feature_flags & (DM_CACHE_FEATURE_WRITEBACK |
				      DM_CACHE_FEATURE_WRITETHROUGH |
				      DM_CACHE_FEATURE_PASSTHROUGH)) ? 1 : 0;
}

const char *get_cache_mode_name(const struct lv_segment *seg)
{
	if (seg->feature_flags & DM_CACHE_FEATURE_WRITEBACK)
		return "writeback";

	if (seg->feature_flags & DM_CACHE_FEATURE_WRITETHROUGH)
		return "writethrough";

	if (seg->feature_flags & DM_CACHE_FEATURE_PASSTHROUGH)
		return "passthrough";

	log_error(INTERNAL_ERROR "LV %s has uknown feature flags %" PRIu64 ".",
		  display_lvname(seg->lv), seg->feature_flags);

	return NULL;
}

int cache_set_mode(struct lv_segment *seg, const char *str)
{
	struct cmd_context *cmd = seg->lv->vg->cmd;
	int id;
	uint64_t mode;

	if (!str && !seg_is_cache(seg))
		return 1;			/* Defaults only for cache */

	if (seg_is_cache(seg))
		seg = first_seg(seg->pool_lv);

	if (!str) {
		if (cache_mode_is_set(seg))
			return 1;               /* Default already set in cache pool */

		id = allocation_cache_mode_CFG;

		/* If present, check backward compatible settings */
		if (!find_config_node(cmd, cmd->cft, id) &&
		    find_config_node(cmd, cmd->cft, allocation_cache_pool_cachemode_CFG))
			id = allocation_cache_pool_cachemode_CFG;

		str = find_config_tree_str(cmd, id, NULL);
	}

	if (!strcmp(str, "writeback"))
		mode = DM_CACHE_FEATURE_WRITEBACK;
	else if (!strcmp(str, "writethrough"))
		mode = DM_CACHE_FEATURE_WRITETHROUGH;
	else if (!strcmp(str, "passthrough"))
		mode = DM_CACHE_FEATURE_PASSTHROUGH;
	else {
		log_error("Cannot set unknown cache mode \"%s\".", str);
		return 0;
	}

	seg->feature_flags &= ~(DM_CACHE_FEATURE_WRITEBACK |
				DM_CACHE_FEATURE_WRITETHROUGH |
				DM_CACHE_FEATURE_PASSTHROUGH);
	seg->feature_flags |= mode;

	return 1;
}

/*
 * At least warn a user if certain cache stacks may present some problems
 */
void cache_check_for_warns(const struct lv_segment *seg)
{
	struct logical_volume *origin_lv = seg_lv(seg, 0);

	if (lv_is_raid(origin_lv) &&
	    first_seg(seg->pool_lv)->feature_flags & DM_CACHE_FEATURE_WRITEBACK)
		log_warn("WARNING: Data redundancy is lost with writeback "
			 "caching of raid logical volume!");

	if (lv_is_thin_pool_data(seg->lv))
		log_warn("WARNING: Cached thin pool's data cannot be currently "
			 "resized and require manual uncache before resize!");
}

int update_cache_pool_params(const struct segment_type *segtype,
			     struct volume_group *vg, unsigned attr,
			     int passed_args, uint32_t pool_data_extents,
			     uint32_t *pool_metadata_extents,
			     int *chunk_size_calc_method, uint32_t *chunk_size)
{
	uint64_t min_meta_size;
	uint32_t extent_size = vg->extent_size;
	uint64_t pool_metadata_size = (uint64_t) *pool_metadata_extents * vg->extent_size;

	if (!(passed_args & PASS_ARG_CHUNK_SIZE))
		*chunk_size = DEFAULT_CACHE_POOL_CHUNK_SIZE * 2;

	if (!validate_pool_chunk_size(vg->cmd, segtype, *chunk_size))
		return_0;

	/*
	 * Default meta size is:
	 * (Overhead + mapping size + hint size)
	 */
	min_meta_size = (uint64_t) pool_data_extents * extent_size / *chunk_size;	/* nr_chunks */
	min_meta_size *= (DM_BYTES_PER_BLOCK + DM_MAX_HINT_WIDTH + DM_HINT_OVERHEAD_PER_BLOCK);
	min_meta_size = (min_meta_size + (SECTOR_SIZE - 1)) >> SECTOR_SHIFT;	/* in sectors */
	min_meta_size += DM_TRANSACTION_OVERHEAD * (1024 >> SECTOR_SHIFT);

	/* Round up to extent size */
	if (min_meta_size % extent_size)
		min_meta_size += extent_size - min_meta_size % extent_size;

	if (!pool_metadata_size)
		pool_metadata_size = min_meta_size;

	if (pool_metadata_size > (2 * DEFAULT_CACHE_POOL_MAX_METADATA_SIZE)) {
		pool_metadata_size = 2 * DEFAULT_CACHE_POOL_MAX_METADATA_SIZE;
		if (passed_args & PASS_ARG_POOL_METADATA_SIZE)
			log_warn("WARNING: Maximum supported pool metadata size is %s.",
				 display_size(vg->cmd, pool_metadata_size));
	} else if (pool_metadata_size < min_meta_size) {
		if (passed_args & PASS_ARG_POOL_METADATA_SIZE)
			log_warn("WARNING: Minimum required pool metadata size is %s "
				 "(needs extra %s).",
				 display_size(vg->cmd, min_meta_size),
				 display_size(vg->cmd, min_meta_size - pool_metadata_size));
		pool_metadata_size = min_meta_size;
	}

	if (!(*pool_metadata_extents =
	      extents_from_size(vg->cmd, pool_metadata_size, extent_size)))
		return_0;

	return 1;
}

/*
 * Validate arguments for converting origin into cached volume with given cache pool.
 *
 * Always validates origin_lv, and when it is known also cache pool_lv
 */
int validate_lv_cache_create_pool(const struct logical_volume *pool_lv)
{
	struct lv_segment *seg;

	if (!lv_is_cache_pool(pool_lv)) {
		log_error("Logical volume %s is not a cache pool.",
			  display_lvname(pool_lv));
		return 0;
	}

	if (lv_is_locked(pool_lv)) {
		log_error("Cannot use locked cache pool %s.",
			  display_lvname(pool_lv));
		return 0;
	}

	if (!dm_list_empty(&pool_lv->segs_using_this_lv)) {
		seg = get_only_segment_using_this_lv(pool_lv);
		log_error("Logical volume %s is already in use by %s",
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
	if (!lv_is_visible(origin_lv) && !lv_is_thin_pool_data(origin_lv)) {
		log_error("Can't convert internal LV %s.", display_lvname(origin_lv));
		return 0;
	}

	/*
	 * Only linear, striped or raid supported.
	 * FIXME Tidy up all these type restrictions.
	 */
	if (lv_is_cache_type(origin_lv) ||
	    lv_is_mirror_type(origin_lv) ||
	    lv_is_thin_volume(origin_lv) || lv_is_thin_pool_metadata(origin_lv) ||
	    lv_is_origin(origin_lv) || lv_is_merging_origin(origin_lv) ||
	    lv_is_cow(origin_lv) || lv_is_merging_cow(origin_lv) ||
	    lv_is_external_origin(origin_lv) ||
	    lv_is_virtual(origin_lv)) {
		log_error("Cache is not supported with %s segment type of the original logical volume %s.",
			  first_seg(origin_lv)->segtype->name, display_lvname(origin_lv));
		return 0;
	}

	return 1;
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
	const struct segment_type *segtype;
	struct cmd_context *cmd = pool_lv->vg->cmd;
	struct logical_volume *cache_lv = origin_lv;
	struct lv_segment *seg;

	if (!validate_lv_cache_create_pool(pool_lv) ||
	    !validate_lv_cache_create_origin(cache_lv))
		return_NULL;

	if (lv_is_thin_pool(cache_lv))
		cache_lv = seg_lv(first_seg(cache_lv), 0); /* cache _tdata */

	if (!(segtype = get_segtype_from_string(cmd, "cache")))
		return_NULL;

	if (!insert_layer_for_lv(cmd, cache_lv, CACHE, "_corig"))
		return_NULL;

	seg = first_seg(cache_lv);
	seg->segtype = segtype;

	if (!attach_pool_lv(seg, pool_lv, NULL, NULL))
		return_NULL;

	return cache_lv;
}

/*
 * lv_cache_remove
 * @cache_lv
 *
 * Given a cache LV, remove the cache layer.  This will unlink
 * the origin and cache_pool, remove the cache LV layer, and promote
 * the origin to a usable non-cached LV of the same name as the
 * given cache_lv.
 *
 * Returns: 1 on success, 0 on failure
 */
int lv_cache_remove(struct logical_volume *cache_lv)
{
	int is_cleaner;
	uint64_t dirty_blocks;
	struct lv_segment *cache_seg = first_seg(cache_lv);
	struct logical_volume *corigin_lv;
	struct logical_volume *cache_pool_lv;
	struct lv_status_cache *status;

	if (!lv_is_cache(cache_lv)) {
		log_error(INTERNAL_ERROR "LV %s is not cache volume.",
			  display_lvname(cache_lv));
		return 0;
	}

	if (lv_is_pending_delete(cache_lv)) {
		log_error(INTERNAL_ERROR "LV %s is already dropped cache volume.",
			  display_lvname(cache_lv));
		goto remove;  /* Already dropped */
	}

	/* Localy active volume is needed for writeback */
	if (!lv_is_active_locally(cache_lv)) {
		/* Give up any remote locks */
		if (!deactivate_lv(cache_lv->vg->cmd, cache_lv)) {
			log_error("Cannot deactivate remotely active cache lv.");
			return 0;
		}
		/* For inactive writethrough just drop cache layer */
		if (first_seg(cache_seg->pool_lv)->feature_flags &
		    DM_CACHE_FEATURE_WRITETHROUGH) {
			corigin_lv = seg_lv(cache_seg, 0);
			if (!detach_pool_lv(cache_seg))
				return_0;
			if (!remove_layer_from_lv(cache_lv, corigin_lv))
				return_0;
			if (!lv_remove(corigin_lv))
				return_0;
			return 1;
		}

		/* Otherwise localy active volume is need to sync dirty blocks */
		cache_lv->status |= LV_TEMPORARY;
		if (!activate_lv_excl_local(cache_lv->vg->cmd, cache_lv) ||
		    !lv_is_active_locally(cache_lv)) {
			log_error("Failed to active cache locally %s.",
				  display_lvname(cache_lv));
			return 0;
		}
		cache_lv->status &= ~LV_TEMPORARY;
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
	if (!lv_cache_status(cache_lv, &status))
		return_0;
	dirty_blocks = status->cache->dirty_blocks;
	if (!(status->cache->feature_flags & DM_CACHE_FEATURE_WRITETHROUGH))
		dirty_blocks++; /* Not writethrough - always dirty */
	is_cleaner = !strcmp(status->cache->policy_name, "cleaner");
	dm_pool_destroy(status->mem);

	if (dirty_blocks && !is_cleaner) {
		/* Switch to cleaner policy to flush the cache */
		log_print_unless_silent("Flushing cache for %s.", cache_lv->name);
		cache_seg->cleaner_policy = 1;
		/* update the kernel to put the cleaner policy in place */
		if (!lv_update_and_reload_origin(cache_lv))
			return_0;
	}

	//FIXME: use polling to do this...
	while (dirty_blocks) {
		if (!lv_cache_status(cache_lv, &status))
			return_0;
		dirty_blocks = status->cache->dirty_blocks;
		dm_pool_destroy(status->mem);
		if (dirty_blocks) {
			log_print_unless_silent(FMTu64 " blocks must still be flushed.",
						dirty_blocks);
			sleep(1);
		}
	}

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

	/* Replace 'error' with 'cache' segtype */
	cache_seg = first_seg(corigin_lv);
	if (!(cache_seg->segtype = get_segtype_from_string(corigin_lv->vg->cmd, "cache")))
		return_0;

	if (!(cache_seg->areas = dm_pool_zalloc(cache_lv->vg->vgmem, sizeof(*cache_seg->areas))))
		return_0;
	if (!set_lv_segment_area_lv(cache_seg, 0, cache_lv, 0, 0))
		return_0;

	cache_seg->area_count = 1;
	corigin_lv->le_count = cache_lv->le_count;
	corigin_lv->size = cache_lv->size;
	corigin_lv->status |= LV_PENDING_DELETE;

	/* Reattach cache pool */
	if (!attach_pool_lv(cache_seg, cache_pool_lv, NULL, NULL))
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
	const struct segment_type *segtype = get_segtype_from_string(cmd, "cache");
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

int cache_set_policy(struct lv_segment *seg, const char *name,
		     const struct dm_config_tree *settings)
{
	struct dm_config_node *cn;
	const struct dm_config_node *cns;
	struct dm_config_tree *old = NULL, *new = NULL, *tmp = NULL;
	int r = 0;
	const int passed_seg_is_cache = seg_is_cache(seg);

	if (passed_seg_is_cache)
		seg = first_seg(seg->pool_lv);

	if (name) {
		if (!(seg->policy_name = dm_pool_strdup(seg->lv->vg->vgmem, name))) {
			log_error("Failed to duplicate policy name.");
			return 0;
		}
	} else if (!seg->policy_name && passed_seg_is_cache) {
		if (!(seg->policy_name = find_config_tree_str(seg->lv->vg->cmd, allocation_cache_policy_CFG, NULL)) &&
		    !(seg->policy_name = _get_default_cache_policy(seg->lv->vg->cmd)))
			return_0;
	}

	if (settings) {
		if (!seg->policy_name) {
			log_error(INTERNAL_ERROR "Can't set policy settings without policy name.");
			return 0;
		}

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
	} else if (passed_seg_is_cache && /* Look for command's profile cache_policies */
		   (cns = find_config_tree_node(seg->lv->vg->cmd, allocation_cache_settings_CFG_SECTION, NULL))) {
		/* Try to find our section for given policy */
		for (cn = cns->child; cn; cn = cn->sib) {
			/* Only matching section names */
			if (cn->v || strcmp(cn->key, seg->policy_name) != 0)
				continue;

			if (!cn->child)
				break;

			if (!(new = dm_config_create()))
				goto_out;

			if (!(new->root = dm_config_clone_node_with_mem(new->mem,
									cn->child, 1)))
				goto_out;

			if (!(seg->policy_settings = dm_config_create_node(new, "policy_settings")))
				goto_out;

			seg->policy_settings->child = new->root;

			break; /* Only first match counts */
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
 * Wipe cache pool metadata area before use.
 *
 * Activates metadata volume as 'cache-pool' so regular wiping
 * of existing visible volume may proceed.
 */
int wipe_cache_pool(struct logical_volume *cache_pool_lv)
{
	int r;

	/* Only unused cache-pool could be activated and wiped */
	if (!lv_is_cache_pool(cache_pool_lv) ||
	    !dm_list_empty(&cache_pool_lv->segs_using_this_lv)) {
		log_error(INTERNAL_ERROR "Failed to wipe cache pool for volume %s.",
			  display_lvname(cache_pool_lv));
		return 0;
	}

	cache_pool_lv->status |= LV_TEMPORARY;
	if (!activate_lv_local(cache_pool_lv->vg->cmd, cache_pool_lv)) {
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
