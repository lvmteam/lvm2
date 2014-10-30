/*
 * Copyright (C) 2014 Red Hat, Inc. All rights reserved.
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

/* https://github.com/jthornber/thin-provisioning-tools/blob/master/caching/cache_metadata_size.cc */
#define DM_TRANSACTION_OVERHEAD		4096  /* KiB */
#define DM_BYTES_PER_BLOCK		16 /* bytes */
#define DM_HINT_OVERHEAD_PER_BLOCK	8  /* bytes */
#define DM_MAX_HINT_WIDTH		(4+16)  /* bytes,  TODO: configurable ?? */

const char *get_cachepool_cachemode_name(const struct lv_segment *seg)
{
	if (seg->feature_flags & DM_CACHE_FEATURE_WRITEBACK)
		return "writeback";

	if (seg->feature_flags & DM_CACHE_FEATURE_WRITETHROUGH)
		return "writethrough";

	return "unknown";
}

int update_cache_pool_params(const struct segment_type *segtype,
			     struct volume_group *vg, unsigned attr,
			     int passed_args, uint32_t pool_data_extents,
			     uint32_t *pool_metadata_extents,
			     int *chunk_size_calc_method, uint32_t *chunk_size)
{
	uint64_t min_meta_size;
	uint32_t extent_size = vg->extent_size;
	uint64_t pool_metadata_size = *pool_metadata_extents * vg->extent_size;

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
 * Cleanup orphan device in the table with temporary activation
 * since in the suspend() we can't deactivate unused nodes
 * and the resume() phase mishandles orphan nodes.
 *
 * TODO: improve libdm to handle this case automatically
 */
static int _cleanup_orphan_lv(struct logical_volume *lv)
{
	lv->status |= LV_TEMPORARY;
	if (!activate_lv(lv->vg->cmd, lv)) {
		log_error("Failed to activate temporary %s", lv->name);
		return 0;
	}
	if (!deactivate_lv(lv->vg->cmd, lv)) {
		log_error("Failed to deactivate temporary %s", lv->name);
		return 0;
	}
	lv->status &= ~LV_TEMPORARY;

	return 1;
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
	const char *policy_name;
	uint64_t dirty_blocks;
	struct lv_segment *cache_seg = first_seg(cache_lv);
	struct logical_volume *corigin_lv;
	struct logical_volume *cache_pool_lv;

	if (!lv_is_cache(cache_lv)) {
		log_error(INTERNAL_ERROR "LV %s is not cached.", cache_lv->name);
		return 0;
	}

	/* Localy active volume is needed (writeback only?) */
	if (!lv_is_active_locally(cache_lv)) {
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
	if (!lv_cache_policy_info(cache_lv, &policy_name, NULL, NULL))
		return_0;

	if (strcmp(policy_name, "cleaner")) {
		/* We must swap in the cleaner to flush the cache */
		log_print_unless_silent("Flushing cache for %s.", cache_lv->name);

		/*
		 * Is there are clean way to free the memory for the name
		 * and argv when changing the policy?
		 */
		cache_seg->policy_name = "cleaner";
		cache_seg->policy_argc = 0;
		cache_seg->policy_argv = NULL;

		/* update the kernel to put the cleaner policy in place */
		if (!lv_update_and_reload(cache_lv))
                        return_0;
	}

	//FIXME: use polling to do this...
	do {
		if (!lv_cache_block_info(cache_lv, NULL,
					 &dirty_blocks, NULL, NULL))
			return_0;
		log_print_unless_silent("%" PRIu64 " blocks must still be flushed.",
					dirty_blocks);
		if (dirty_blocks)
			sleep(1);
	} while (dirty_blocks);

	cache_pool_lv = cache_seg->pool_lv;
	if (!detach_pool_lv(cache_seg))
		return_0;

	/* Regular LV which user may remove if there are problems */
	corigin_lv = seg_lv(cache_seg, 0);
	lv_set_visible(corigin_lv);
	if (!remove_layer_from_lv(cache_lv, corigin_lv))
			return_0;

	if (!lv_update_and_reload(cache_lv))
		return_0;

	/*
	 * suspend_lv on this cache LV suspends all components:
	 * - the top-level cache LV
	 * - the origin
	 * - the cache_pool _cdata and _cmeta
	 *
	 * resume_lv on this (former) cache LV will resume all
	 *
	 * FIXME: currently we can't easily avoid execution of
	 * blkid on resumed error device
	 */

	/*
	 * cleanup orphan devices
	 *
	 * FIXME:
	 * fix _add_dev() to support this case better
	 * since that should be handled internally by resume_lv()
	 * which should autoremove any orphans
	 */
	if (!_cleanup_orphan_lv(corigin_lv))  /* _corig */
		return_0;
	if (!_cleanup_orphan_lv(seg_lv(first_seg(cache_pool_lv), 0))) /* _cdata */
		return_0;
	if (!_cleanup_orphan_lv(first_seg(cache_pool_lv)->metadata_lv)) /* _cmeta */
		return_0;

	if (!lv_remove(corigin_lv))
		return_0;

	return 1;
}

int get_cache_mode(const char *str, uint32_t *flags)
{
	if (!strcmp(str, "writethrough"))
		*flags |= DM_CACHE_FEATURE_WRITETHROUGH;
	else if (!strcmp(str, "writeback"))
		*flags |= DM_CACHE_FEATURE_WRITEBACK;
	else {
		log_error("Cache pool cachemode \"%s\" is unknown.", str);
		return 0;
	}

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
	return seg && lv_is_cache(seg->lv) && (seg_lv(seg, 0) == lv);
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
