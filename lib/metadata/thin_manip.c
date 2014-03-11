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
#include "defaults.h"
#include "display.h"

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
	percent_t percent;
	int threshold = PERCENT_1 *
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

	if ((external_lv->size < csize) || (external_lv->size % csize)) {
		/* TODO: Validate with thin feature flag once, it will be supported */
		log_error("Can't use \"%s/%s\" as external origin with \"%s/%s\" pool. "
			  "Size %s is not a multiple of pool's chunk size %s.",
			  external_lv->vg->name, external_lv->name,
			  pool_seg->lv->vg->name, pool_seg->lv->name,
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

	return 1;
}

int update_profilable_pool_params(struct cmd_context *cmd, struct profile *profile,
				  int passed_args, int *chunk_size_calc_method,
				  uint32_t *chunk_size, thin_discards_t *discards,
				  int *zero)
{
	const char *str;

	if (!(passed_args & PASS_ARG_CHUNK_SIZE)) {
		if (!(*chunk_size = find_config_tree_int(cmd, allocation_thin_pool_chunk_size_CFG, profile) * 2)) {
			str = find_config_tree_str(cmd, allocation_thin_pool_chunk_size_policy_CFG, profile);
			if (!strcasecmp(str, "generic"))
				*chunk_size_calc_method = THIN_CHUNK_SIZE_CALC_METHOD_GENERIC;
			else if (!strcasecmp(str, "performance"))
				*chunk_size_calc_method = THIN_CHUNK_SIZE_CALC_METHOD_PERFORMANCE;
			else {
				log_error("Thin pool chunk size calculation policy \"%s\" is unrecognised.", str);
				return 0;
			}
			*chunk_size = get_default_allocation_thin_pool_chunk_size_CFG(cmd, profile) * 2;
		}
	}

	if ((*chunk_size < DM_THIN_MIN_DATA_BLOCK_SIZE) ||
	    (*chunk_size > DM_THIN_MAX_DATA_BLOCK_SIZE)) {
		log_error("Chunk size must be in the range %s to %s.",
			  display_size(cmd, DM_THIN_MIN_DATA_BLOCK_SIZE),
			  display_size(cmd, DM_THIN_MAX_DATA_BLOCK_SIZE));
		return 0;
	}

	if (!(passed_args & PASS_ARG_DISCARDS)) {
		str = find_config_tree_str(cmd, allocation_thin_pool_discards_CFG, profile);
		if (!get_pool_discards(str, discards))
			return_0;
	}

	if (!(passed_args & PASS_ARG_ZERO))
		*zero = find_config_tree_bool(cmd, allocation_thin_pool_zero_CFG, profile);

	return 1;
}

int update_thin_pool_params(struct volume_group *vg, unsigned attr,
			    int passed_args,
			    uint32_t data_extents, uint32_t extent_size,
			    int *chunk_size_calc_method, uint32_t *chunk_size,
			    thin_discards_t *discards,
			    uint64_t *pool_metadata_size, int *zero)
{
	size_t estimate_chunk_size;
	struct cmd_context *cmd = vg->cmd;

	if (!update_profilable_pool_params(cmd, vg->profile, passed_args,
					   chunk_size_calc_method, chunk_size,
					   discards, zero))
		return_0;

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

struct logical_volume *alloc_pool_metadata(struct logical_volume *pool_lv,
					   const char *name, uint32_t read_ahead,
					   uint32_t stripes, uint32_t stripe_size,
					   uint64_t size, alloc_policy_t alloc,
					   struct dm_list *pvh)
{
	struct logical_volume *metadata_lv;
	/* FIXME: Make lvm2api usable */
	struct lvcreate_params lvc = {
		.activate = CHANGE_ALY,
		.alloc = alloc,
		.lv_name = name,
		.major = -1,
		.minor = -1,
		.permission = LVM_READ | LVM_WRITE,
		.pvh = pvh,
		.read_ahead = read_ahead,
		.stripe_size = stripe_size,
		.stripes = stripes,
		.vg_name = pool_lv->vg->name,
		.zero = 1,
	};

	dm_list_init(&lvc.tags);

	if (!(lvc.extents = extents_from_size(pool_lv->vg->cmd, size,
					      pool_lv->vg->extent_size)))
		return_0;

	if (!(lvc.segtype = get_segtype_from_string(pool_lv->vg->cmd, "striped")))
		return_0;

	/* FIXME: allocate properly space for metadata_lv */

	if (!(metadata_lv = lv_create_single(pool_lv->vg, &lvc)))
		return_0;

	return metadata_lv;
}

static struct logical_volume *_alloc_pool_metadata_spare(struct volume_group *vg,
							 uint32_t extents,
							 struct dm_list *pvh)
{
	struct logical_volume *lv;

	/* FIXME: Make lvm2api usable */
	struct lvcreate_params lp = {
		.activate = CHANGE_ALY,
		.alloc = ALLOC_INHERIT,
		.extents = extents,
		.major = -1,
		.minor = -1,
		.permission = LVM_READ | LVM_WRITE,
		.pvh = pvh ? : &vg->pvs,
		.read_ahead = DM_READ_AHEAD_AUTO,
		.stripes = 1,
		.vg_name = vg->name,
		.zero = 1,
		.temporary = 1,
	};

	dm_list_init(&lp.tags);

	if (!(lp.segtype = get_segtype_from_string(vg->cmd, "striped")))
		return_0;

	/* FIXME: Maybe using silent mode ? */
	if (!(lv = lv_create_single(vg, &lp)))
		return_0;

	/* Spare LV should not be active */
	if (!deactivate_lv_local(vg->cmd, lv)) {
		log_error("Unable to deactivate pool metadata spare LV. "
			  "Manual intervention required.");
		return 0;
	}

	if (!vg_set_pool_metadata_spare(lv))
		return_0;

	return lv;
}

/*
 * Create/resize pool metadata spare LV
 * Caller does vg_write(), vg_commit() with pool creation
 * extents is 0, max size is determined
 */
int handle_pool_metadata_spare(struct volume_group *vg, uint32_t extents,
			       struct dm_list *pvh, int poolmetadataspare)
{
	struct logical_volume *lv = vg->pool_metadata_spare_lv;
	uint32_t seg_mirrors;
	struct lv_segment *seg;
	const struct lv_list *lvl;

	if (!extents)
		/* Find maximal size of metadata LV */
		dm_list_iterate_items(lvl, &vg->lvs)
			if (lv_is_thin_pool_metadata(lvl->lv) &&
			    (lvl->lv->le_count > extents))
				extents = lvl->lv->le_count;

	if (!poolmetadataspare) {
		/* TODO: Not showing when lvm.conf would define 'n' ? */
		if (DEFAULT_POOL_METADATA_SPARE && extents)
			/* Warn if there would be any user */
			log_warn("WARNING: recovery of pools without pool "
				 "metadata spare LV is not automated.");
		return 1;
	}

	if (!lv) {
		if (!_alloc_pool_metadata_spare(vg, extents, pvh))
			return_0;

		return 1;
	}

	seg = last_seg(lv);
	seg_mirrors = lv_mirror_count(lv);

	/* Check spare LV is big enough and preserve segtype */
	if ((lv->le_count < extents) && seg &&
	    !lv_extend(lv, seg->segtype,
		       seg->area_count / seg_mirrors,
		       seg->stripe_size,
		       seg_mirrors,
		       seg->region_size,
		       extents - lv->le_count, NULL,
		       pvh, lv->alloc, 0))
		return_0;

	return 1;
}

int vg_set_pool_metadata_spare(struct logical_volume *lv)
{
	char new_name[NAME_LEN];
	struct volume_group *vg = lv->vg;

	if (vg->pool_metadata_spare_lv) {
		if (vg->pool_metadata_spare_lv == lv)
			return 1;
		if (!vg_remove_pool_metadata_spare(vg))
			return_0;
	}

	if (dm_snprintf(new_name, sizeof(new_name), "%s_pmspare", lv->name) < 0) {
		log_error("Can't create pool metadata spare. Name of pool LV "
			  "%s is too long.", lv->name);
		return 0;
	}

	if (!lv_rename_update(vg->cmd, lv, new_name, 0))
		return_0;

	lv_set_hidden(lv);
	lv->status |= POOL_METADATA_SPARE;
	vg->pool_metadata_spare_lv = lv;

	return 1;
}

int vg_remove_pool_metadata_spare(struct volume_group *vg)
{
	char new_name[NAME_LEN];
	char *c;

	struct logical_volume *lv = vg->pool_metadata_spare_lv;

	if (!(lv->status & POOL_METADATA_SPARE)) {
		log_error(INTERNAL_ERROR "LV %s is not pool metadata spare.",
			  lv->name);
		return 0;
	}

	vg->pool_metadata_spare_lv = NULL;
	lv->status &= ~POOL_METADATA_SPARE;
	lv_set_visible(lv);

	/* Cut off suffix _pmspare */
	(void) dm_strncpy(new_name, lv->name, sizeof(new_name));
	if (!(c = strchr(new_name, '_'))) {
		log_error(INTERNAL_ERROR "LV %s has no suffix for pool metadata spare.",
			  new_name);
		return 0;
	}
	*c = 0;

	/* If the name is in use, generate new lvol%d */
	if (find_lv_in_vg(vg, new_name) &&
	    !generate_lv_name(vg, "lvol%d", new_name, sizeof(new_name))) {
		log_error("Failed to generate unique name for "
			  "pool metadata spare logical volume.");
		return 0;
	}

	log_print_unless_silent("Renaming existing pool metadata spare "
				"logical volume \"%s/%s\" to \"%s/%s\".",
                                vg->name, lv->name, vg->name, new_name);

	if (!lv_rename_update(vg->cmd, lv, new_name, 0))
		return_0;

	/* To display default warning */
	(void) handle_pool_metadata_spare(vg, 0, 0, 0);

	return 1;
}
