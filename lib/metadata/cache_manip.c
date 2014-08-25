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

int update_cache_pool_params(struct volume_group *vg, unsigned attr,
			     int passed_args, uint32_t data_extents,
			     uint64_t *pool_metadata_size,
			     int *chunk_size_calc_method, uint32_t *chunk_size)
{
	uint64_t min_meta_size;

	if (!(passed_args & PASS_ARG_CHUNK_SIZE))
		*chunk_size = DEFAULT_CACHE_POOL_CHUNK_SIZE * 2;

	if ((*chunk_size < DM_CACHE_MIN_DATA_BLOCK_SIZE) ||
	    (*chunk_size > DM_CACHE_MAX_DATA_BLOCK_SIZE)) {
		log_error("Chunk size must be in the range %s to %s.",
			  display_size(vg->cmd, DM_CACHE_MIN_DATA_BLOCK_SIZE),
			  display_size(vg->cmd, DM_CACHE_MAX_DATA_BLOCK_SIZE));
		return 0;
	}

	if (*chunk_size & (DM_CACHE_MIN_DATA_BLOCK_SIZE - 1)) {
		log_error("Chunk size must be a multiple of %s.",
			  display_size(vg->cmd, DM_CACHE_MIN_DATA_BLOCK_SIZE));
		return 0;
	}

	/*
	 * Default meta size is:
	 * (4MiB + (16 Bytes for each chunk-sized block))
	 * ... plus a good amount of padding (2x) to cover any
	 * policy hint data that may be added in the future.
	 */
	min_meta_size = (uint64_t)data_extents * vg->extent_size * 16;
	min_meta_size /= *chunk_size; /* # of Bytes we need */
	min_meta_size *= 2;              /* plus some padding */
	min_meta_size /= 512;            /* in sectors */
	min_meta_size += 4*1024*2;       /* plus 4MiB */

	if (!*pool_metadata_size)
		*pool_metadata_size = min_meta_size;

	if (*pool_metadata_size > (2 * DEFAULT_CACHE_POOL_MAX_METADATA_SIZE)) {
		*pool_metadata_size = 2 * DEFAULT_CACHE_POOL_MAX_METADATA_SIZE;
		if (passed_args & PASS_ARG_POOL_METADATA_SIZE)
			log_warn("WARNING: Maximum supported pool metadata size is %s.",
				 display_size(vg->cmd, *pool_metadata_size));
	} else if (*pool_metadata_size < min_meta_size) {
		if (passed_args & PASS_ARG_POOL_METADATA_SIZE)
			log_warn("WARNING: Minimum supported pool metadata size is %s "
				 "(needs extra %s).",
				 display_size(vg->cmd, min_meta_size),
				 display_size(vg->cmd, min_meta_size - *pool_metadata_size));
		*pool_metadata_size = min_meta_size;
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
struct logical_volume *lv_cache_create(struct logical_volume *pool,
				       struct logical_volume *origin)
{
	const struct segment_type *segtype;
	struct cmd_context *cmd = pool->vg->cmd;
	struct logical_volume *cache_lv;
	struct lv_segment *seg;

	if (!lv_is_cache_pool(pool)) {
		log_error(INTERNAL_ERROR
			  "%s is not a cache_pool LV", pool->name);
		return NULL;
	}

	if (!dm_list_empty(&pool->segs_using_this_lv)) {
		seg = get_only_segment_using_this_lv(pool);
		log_error("%s is already in use by %s",
			  pool->name, seg ? seg->lv->name : "another LV");
		return NULL;
	}

	if (lv_is_cache_type(origin)) {
		/*
		 * FIXME: We can layer caches, insert_layer_for_lv() would
		 * have to do a better job renaming the LVs in the stack
		 * first so that there isn't a name collision with <name>_corig.
		 * The origin under the origin would become *_corig_corig
		 * before renaming the origin above to *_corig.
		 */
		log_error("Creating a cache LV from an existing cache LV is"
			  "not yet supported.");
		return NULL;
	}

	if (!(segtype = get_segtype_from_string(cmd, "cache")))
		return_NULL;

	cache_lv = origin;
	if (!(origin = insert_layer_for_lv(cmd, cache_lv, CACHE, "_corig")))
		return_NULL;

	seg = first_seg(cache_lv);
	seg->segtype = segtype;

	if (!attach_pool_lv(seg, pool, NULL, NULL))
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
	struct cmd_context *cmd = cache_lv->vg->cmd;
	const char *policy_name;
	uint64_t dirty_blocks;
	struct lv_segment *cache_seg = first_seg(cache_lv);
	struct logical_volume *corigin_lv;
	struct logical_volume *cache_pool_lv;

	if (!lv_is_cache(cache_lv)) {
		log_error(INTERNAL_ERROR "LV %s is not cached.", cache_lv->name);
		return 0;
	}

	/* Active volume is needed (writeback only?) */
	if (!lv_is_active_locally(cache_lv) &&
	    !activate_lv_excl_local(cache_lv->vg->cmd, cache_lv)) {
		log_error("Failed to active cache locally %s.", cache_lv->name);
		return 0;
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
		if (!vg_write(cache_lv->vg))
			return_0;
		if (!suspend_lv(cmd, cache_lv))
			return_0;
		if (!vg_commit(cache_lv->vg))
			return_0;
		if (!resume_lv(cmd, cache_lv))
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

	if (!vg_write(cache_lv->vg))
		return_0;

	/*
	 * suspend_lv on this cache LV suspends all components:
	 * - the top-level cache LV
	 * - the origin
	 * - the cache_pool _cdata and _cmeta
	 */
	if (!suspend_lv(cmd, cache_lv))
		return_0;

	if (!vg_commit(cache_lv->vg))
		return_0;

	/* resume_lv on this (former) cache LV will resume all */
	/*
	 * FIXME: currently we can't easily avoid execution of
	 * blkid on resumed error device
	 */
	if (!resume_lv(cmd, cache_lv))
		return_0;

	/*
	 * cleanup orphan devices
	 *
	 * FIXME:
	 * fix _add_dev() to support this case better
	 * since the should be handled interanlly by resume_lv()
	 * which should autoremove any orhpans
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
