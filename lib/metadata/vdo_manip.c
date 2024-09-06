/*
 * Copyright (C) 2018-2019 Red Hat, Inc. All rights reserved.
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
#include "lib/misc/lvm-exec.h"
#include "lib/metadata/lv_alloc.h"

#include <sys/sysinfo.h> // sysinfo
#include <stdarg.h>

const char *get_vdo_compression_state_name(enum dm_vdo_compression_state state)
{
	switch (state) {
	case DM_VDO_COMPRESSION_ONLINE:
		return "online";
	default:
		log_debug(INTERNAL_ERROR "Unrecognized compression state: %u.", state);
		/* Fall through */
	case DM_VDO_COMPRESSION_OFFLINE:
		return "offline";
	}
}

const char *get_vdo_index_state_name(enum dm_vdo_index_state state)
{
	switch (state) {
	case DM_VDO_INDEX_ERROR:
		return "error";
	case DM_VDO_INDEX_CLOSED:
		return "closed";
	case DM_VDO_INDEX_OPENING:
		return "opening";
	case DM_VDO_INDEX_CLOSING:
		return "closing";
	case DM_VDO_INDEX_OFFLINE:
		return "offline";
	case DM_VDO_INDEX_ONLINE:
		return "online";
	default:
		log_debug(INTERNAL_ERROR "Unrecognized index state: %u.", state);
		/* Fall through */
	case DM_VDO_INDEX_UNKNOWN:
		return "unknown";
	}
}

const char *get_vdo_operating_mode_name(enum dm_vdo_operating_mode mode)
{
	switch (mode) {
	case DM_VDO_MODE_RECOVERING:
		return "recovering";
	case DM_VDO_MODE_READ_ONLY:
		return "read-only";
	default:
		log_debug(INTERNAL_ERROR "Unrecognized operating mode: %u.", mode);
		/* Fall through */
	case DM_VDO_MODE_NORMAL:
		return "normal";
	}
}

const char *get_vdo_write_policy_name(enum dm_vdo_write_policy policy)
{
	switch (policy) {
	case DM_VDO_WRITE_POLICY_SYNC:
		return "sync";
	case DM_VDO_WRITE_POLICY_ASYNC:
		return "async";
	case DM_VDO_WRITE_POLICY_ASYNC_UNSAFE:
		return "async-unsafe";
	default:
		log_debug(INTERNAL_ERROR "Unrecognized VDO write policy: %u.", policy);
		/* Fall through */
	case DM_VDO_WRITE_POLICY_AUTO:
		return "auto";
	}
}

/*
 * Size of VDO virtual LV is adding header_size in front and back of device
 * to avoid collision with blkid checks.
 */
static uint64_t _get_virtual_size(uint32_t extents, uint32_t extent_size,
				  uint32_t header_size)
{
	return (uint64_t) extents * extent_size + 2 * header_size;
}

uint64_t get_vdo_pool_virtual_size(const struct lv_segment *vdo_pool_seg)
{
	return _get_virtual_size(vdo_pool_seg->vdo_pool_virtual_extents,
				 vdo_pool_seg->lv->vg->extent_size,
				 vdo_pool_seg->vdo_pool_header_size);
}

int update_vdo_pool_virtual_size(struct lv_segment *vdo_pool_seg)
{
	struct seg_list *sl;
	uint32_t extents = 0;

	/* FIXME: as long as we have only SINGLE VDO with vdo-pool this works */
	/* after adding support for multiple VDO LVs - this needs heavy rework */
	dm_list_iterate_items(sl, &vdo_pool_seg->lv->segs_using_this_lv)
		extents += sl->seg->len;

	/* Only growing virtual/logical VDO size */
	if (extents > vdo_pool_seg->vdo_pool_virtual_extents)
		vdo_pool_seg->vdo_pool_virtual_extents = extents;

	return 1;
}

uint32_t get_vdo_pool_max_extents(const struct dm_vdo_target_params *vtp,
				  uint32_t extent_size)
{
	uint64_t max_extents = (DM_VDO_PHYSICAL_SIZE_MAXIMUM + extent_size - 1) / extent_size;
	uint64_t max_slab_extents = ((extent_size - 1 + DM_VDO_SLABS_MAXIMUM *
				      ((uint64_t)vtp->slab_size_mb << (20 - SECTOR_SHIFT))) /
				     extent_size);

	max_extents = (max_slab_extents < max_extents) ? max_slab_extents : max_extents;

	return (max_extents > UINT32_MAX) ? UINT32_MAX : (uint32_t)max_extents;
}


static int _sysfs_get_kvdo_value(const char *dm_name, const struct dm_info *dminfo,
				 const char *vdo_param, uint64_t *value)
{
	char path[PATH_MAX];
	char temp[64];
	int fd, size, r = 0;

	if (dm_snprintf(path, sizeof(path), "%sblock/dm-%d/vdo/%s",
			dm_sysfs_dir(), dminfo->minor, vdo_param) < 0) {
		log_debug("Failed to build kvdo path.");
		return 0;
	}

	if ((fd = open(path, O_RDONLY)) < 0) {
		/* try with older location */
		if (dm_snprintf(path, sizeof(path), "%skvdo/%s/%s",
				dm_sysfs_dir(), dm_name, vdo_param) < 0) {
			log_debug("Failed to build kvdo path.");
			return 0;
		}

		if ((fd = open(path, O_RDONLY)) < 0) {
			log_sys_debug("open", path);
			goto bad;
		}
	}

	if ((size = read(fd, temp, sizeof(temp) - 1)) < 0) {
		log_sys_debug("read", path);
		goto bad;
	}
	temp[size] = 0;
	errno = 0;
	*value = strtoll(temp, NULL, 0);
	if (errno) {
		log_sys_debug("strtool", path);
		goto bad;
	}

	r = 1;
bad:
	if (fd >= 0 && close(fd))
		log_sys_debug("close", path);

	return r;
}

int parse_vdo_pool_status(struct dm_pool *mem, const struct logical_volume *vdo_pool_lv,
			  const char *params, const struct dm_info *dminfo,
			  struct lv_status_vdo *status)
{
	struct dm_vdo_status_parse_result result;
	char *dm_name;
	uint64_t blocks;

	status->usage = DM_PERCENT_INVALID;
	status->saving = DM_PERCENT_INVALID;
	status->data_usage = DM_PERCENT_INVALID;

	if (!(dm_name = dm_build_dm_name(mem, vdo_pool_lv->vg->name,
					 vdo_pool_lv->name, lv_layer(vdo_pool_lv)))) {
		log_error("Failed to build VDO DM name %s.",
			  display_lvname(vdo_pool_lv));
		return 0;
	}

	if (!dm_vdo_status_parse(mem, params, &result)) {
		log_error("Cannot parse %s VDO pool status %s.",
			  display_lvname(vdo_pool_lv), result.error);
		return 0;
	}

	status->vdo = result.status;

	if ((result.status->operating_mode == DM_VDO_MODE_NORMAL) &&
            ((status->data_blocks_used != ULLONG_MAX) ||
	     _sysfs_get_kvdo_value(dm_name, dminfo, "statistics/data_blocks_used",
				   &status->data_blocks_used)) &&
	    ((status->logical_blocks_used != ULLONG_MAX) ||
	     _sysfs_get_kvdo_value(dm_name, dminfo, "statistics/logical_blocks_used",
				   &status->logical_blocks_used))) {
		status->usage = dm_make_percent(result.status->used_blocks,
						result.status->total_blocks);
		status->saving = dm_make_percent(status->logical_blocks_used - status->data_blocks_used,
						 status->logical_blocks_used);
		/* coverity needs to use a local variable to handle check here */
		status->data_usage = dm_make_percent(((blocks = status->data_blocks_used) < (ULLONG_MAX / DM_VDO_BLOCK_SIZE)) ?
						     (blocks * DM_VDO_BLOCK_SIZE) : ULLONG_MAX,
						     first_seg(vdo_pool_lv)->vdo_pool_virtual_extents *
						     (uint64_t) vdo_pool_lv->vg->extent_size);
	}

	return 1;
}


/*
 * Formats data LV for a use as a VDO pool LV.
 *
 * Calls tool 'vdoformat' on the already active volume.
 */
static int _format_vdo_pool_data_lv(struct logical_volume *data_lv,
				    const struct dm_vdo_target_params *vtp,
				    uint64_t *logical_size)
{
	char *dpath, *c;
	struct pipe_data pdata;
	uint64_t logical_size_aligned = 1;
	FILE *f;
	uint64_t lb;
	unsigned slabbits;
	unsigned reformatting = 0;
	int args = 0;
	char buf[256]; /* buffer for short disk header (64B) */
	char *buf_pos = buf;
	const char *argv[DEFAULT_MAX_EXEC_ARGS + 9] = { /* Max supported args */
		find_config_tree_str_allow_empty(data_lv->vg->cmd, global_vdo_format_executable_CFG, NULL)
	};

	if (!prepare_exec_args(data_lv->vg->cmd, argv, &args, global_vdo_format_options_CFG))
		return_0;

	if (!(dpath = lv_path_dup(data_lv->vg->cmd->mem, data_lv))) {
		log_error("Failed to build device path for VDO formatting of data volume %s.",
			  display_lvname(data_lv));
		return 0;
	}

	if (*logical_size) {
		logical_size_aligned = 0;

		argv[++args] = buf_pos;
		buf_pos += 1 + dm_snprintf(buf_pos, 30, "--logical-size=" FMTu64 "K",
					   (*logical_size / 2));
	}

	slabbits = 31 - clz(vtp->slab_size_mb / DM_VDO_BLOCK_SIZE * 2 * 1024);  /* to KiB / block_size */
	log_debug("Slab size %s converted to %u bits.",
		  display_size(data_lv->vg->cmd, vtp->slab_size_mb * UINT64_C(2 * 1024)), slabbits);

	argv[++args] = buf_pos;
	buf_pos += 1 + dm_snprintf(buf_pos, 30, "--slab-bits=%u", slabbits);

	/* Convert size to GiB units or one of these strings: 0.25, 0.50, 0.75 */
	argv[++args] = buf_pos;
	if (vtp->index_memory_size_mb >= 1024)
		buf_pos += 1 + dm_snprintf(buf_pos, 30, "--uds-memory-size=%u",
					   vtp->index_memory_size_mb / 1024);
	else
		buf_pos += 1 + dm_snprintf(buf_pos, 30, "--uds-memory-size=0.%2u",
					   (vtp->index_memory_size_mb < 512) ? 25 :
					   (vtp->index_memory_size_mb < 768) ? 50 : 75);

	if (vtp->use_sparse_index)
		argv[++args] = "--uds-sparse";

	/* Only unused VDO data LV could be activated and wiped */
	if (!dm_list_empty(&data_lv->segs_using_this_lv)) {
		log_error(INTERNAL_ERROR "Failed to wipe logical VDO data for volume %s.",
			  display_lvname(data_lv));
		return 0;
	}

	argv[args] = dpath;

	if (!(f = pipe_open(data_lv->vg->cmd, argv, 0, &pdata))) {
		log_error("WARNING: Cannot read output from %s.", argv[0]);
		return 0;
	}

	while (!feof(f) && fgets(buf, sizeof(buf), f)) {
		/* TODO: Watch out for locales */
		if (!*logical_size)
			if (sscanf(buf, "Logical blocks defaulted to " FMTu64 " blocks", &lb) == 1) {
				*logical_size = lb * DM_VDO_BLOCK_SIZE;
				log_verbose("Available VDO logical blocks " FMTu64 " (%s).",
					    lb, display_size(data_lv->vg->cmd, *logical_size));
			}
		if ((c = strchr(buf, '\n')))
			*c = 0; /* cut last '\n' away */
		if (buf[0]) {
			if (reformatting)
				log_verbose("  %s", buf); /* Print vdo_format messages */
			else
				log_print_unless_silent("  %s", buf); /* Print vdo_format messages */
		}
	}

	if (!pipe_close(&pdata)) {
		log_error("Command %s failed.", argv[0]);
		return 0;
	}

	if (!*logical_size) {
		log_error("Number of VDO logical blocks was not provided by vdo_format output.");
		return 0;
	}

	if (logical_size_aligned) {
		// align obtained size to extent size
		logical_size_aligned = *logical_size / data_lv->vg->extent_size * data_lv->vg->extent_size;
		if (*logical_size != logical_size_aligned) {
			log_debug("Using bigger VDO virtual size unaligned on extent size by %s.",
				  display_size(data_lv->vg->cmd, *logical_size - logical_size_aligned));
		}
	}

	return 1;
}

/*
 * convert_vdo_pool_lv
 * @data_lv
 * @vtp
 * @virtual_extents
 *
 * Convert given data LV and its target parameters into a VDO LV with VDO pool.
 *
 * Returns: old data LV on success (passed data LV becomes VDO LV), NULL on failure
 */
int convert_vdo_pool_lv(struct logical_volume *data_lv,
			const struct dm_vdo_target_params *vtp,
			uint32_t *virtual_extents,
			int format,
			uint64_t vdo_pool_header_size)
{
	const uint32_t extent_size = data_lv->vg->extent_size;
	struct cmd_context *cmd = data_lv->vg->cmd;
	struct logical_volume *vdo_pool_lv = data_lv;
	const struct segment_type *vdo_pool_segtype;
	struct lv_segment *vdo_pool_seg;
	uint64_t vdo_logical_size = 0;
	uint64_t adjust;

	if (!(vdo_pool_segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_VDO_POOL)))
		return_0;

	adjust = (*virtual_extents * (uint64_t) extent_size) % DM_VDO_BLOCK_SIZE;
	if (adjust) {
		*virtual_extents += (DM_VDO_BLOCK_SIZE - adjust) / extent_size;
		log_print_unless_silent("Rounding size up to 4,00 KiB VDO logical extent boundary: %s.",
					display_size(data_lv->vg->cmd, *virtual_extents * (uint64_t) extent_size));
	}

	if (*virtual_extents)
		vdo_logical_size =
			_get_virtual_size(*virtual_extents, extent_size, vdo_pool_header_size);

	if (!dm_vdo_validate_target_params(vtp, vdo_logical_size))
		return_0;

	/* Format data LV as VDO volume */
	if (format) {
		if (test_mode()) {
			log_verbose("Test mode: Skipping formatting of VDO pool volume.");
		} else if (!_format_vdo_pool_data_lv(data_lv, vtp, &vdo_logical_size)) {
			log_error("Cannot format VDO pool volume %s.", display_lvname(data_lv));
			return 0;
		}
	} else {
		log_verbose("Skipping VDO formatting %s.", display_lvname(data_lv));
		/* TODO: parse existing VDO data and retrieve vdo_logical_size */
		if (!*virtual_extents)
			vdo_logical_size = data_lv->size;
	}

	if (!deactivate_lv(data_lv->vg->cmd, data_lv)) {
		log_error("Cannot deactivate formatted VDO pool volume %s.",
			  display_lvname(data_lv));
		return 0;
	}

	vdo_logical_size -= 2 * vdo_pool_header_size;

	if (vdo_logical_size < extent_size) {
		if (!*virtual_extents)
			/* User has not specified size and at least 1 extent is necessary */
			log_error("Cannot create fully fitting VDO volume, "
				  "--virtualsize has to be specified.");

		log_error("Size %s for VDO volume cannot be smaller then extent size %s.",
			  display_size(data_lv->vg->cmd, vdo_logical_size),
			  display_size(data_lv->vg->cmd, extent_size));
		return 0;
	}

	*virtual_extents = vdo_logical_size / extent_size;

	/* Move segments from existing data_lv into LV_vdata */
	/* coverity[format_string_injection] lv name is already validated */
	if (!(data_lv = insert_layer_for_lv(cmd, vdo_pool_lv, 0, "_vdata")))
		return_0;

	vdo_pool_seg = first_seg(vdo_pool_lv);
	vdo_pool_seg->segtype = vdo_pool_segtype;
	vdo_pool_seg->vdo_params = *vtp;
	vdo_pool_seg->vdo_pool_header_size = vdo_pool_header_size;
	vdo_pool_seg->vdo_pool_virtual_extents = *virtual_extents;

	vdo_pool_lv->status |= LV_VDO_POOL;
	data_lv->status |= LV_VDO_POOL_DATA;

	return 1;
}

/*
 * Convert LV into vdopool data LV and build virtual VDO LV on top of it.
 * After this it swaps these two LVs so the returned LV is VDO LV!
 */
struct logical_volume *convert_vdo_lv(struct logical_volume *lv,
				      const struct vdo_convert_params *vcp)
{
	struct cmd_context *cmd = lv->vg->cmd;
	char vdopool_name[NAME_LEN], vdopool_tmpl[NAME_LEN];
	struct lvcreate_params lvc = {
		.activate = vcp->activate,
		.alloc = ALLOC_INHERIT,
		.lv_name = vcp->lv_name ? : lv->name, /* preserve the name */
		.major = -1,
		.minor = -1,
		.permission = LVM_READ | LVM_WRITE,
		.pool_name = vdopool_name,
		.pvh = &lv->vg->pvs,
		.read_ahead = DM_READ_AHEAD_AUTO,
		.stripes = 1,
		.suppress_zero_warn = 1, /* suppress warning for this VDO */
		.tags = DM_LIST_HEAD_INIT(lvc.tags),
		.virtual_extents = vcp->virtual_extents ? : lv->le_count, /* same size for Pool and Virtual LV */
	};
	struct logical_volume *vdo_lv, tmp_lv = {
		.segments = DM_LIST_HEAD_INIT(tmp_lv.segments)
	};

	if (!(lvc.segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_VDO)))
		return_NULL;

	if (activation() &&
	    lvc.segtype->ops->target_present &&
	    !lvc.segtype->ops->target_present(cmd, NULL, &lvc.target_attr)) {
		log_error("%s: Required device-mapper target(s) not detected in your kernel.",
			  lvc.segtype->name);
		return NULL;
	}

	if (!vcp->lv_name) {
		/* TODO: maybe  _vpool would be sufficient */
		if (dm_snprintf(vdopool_tmpl, sizeof(vdopool_tmpl), "%s_vpool%%d", lv->name) < 0) {
			log_error("Can't prepare vdo pool name for %s.", display_lvname(lv));
			return NULL;
		}

		if (!generate_lv_name(lv->vg, vdopool_tmpl, vdopool_name, sizeof(vdopool_name))) {
			log_error("Can't generate new name for %s.", vdopool_tmpl);
			return NULL;
		}

		/* Rename to use _vpool name and release the passed-in name here */
		if (!lv_rename_update(cmd, lv, vdopool_name, 1))
			return_NULL;
	} else
		lvc.pool_name = lv->name;

	if (!activate_lv(cmd, lv)) {
		log_error("Aborting. Failed to activate pool metadata %s.",
			  display_lvname(lv));
		return NULL;
	}

	if (vcp->do_zero) {
		if (test_mode()) {
			log_verbose("Test mode: Skipping activation, zeroing and signature wiping.");
		} else if (!(wipe_lv(lv, (struct wipe_params)
				     {
					     .do_zero = 1,
					     .do_wipe_signatures = vcp->do_wipe_signatures,
					     .yes = vcp->yes,
					     .force = vcp->force
				     }))) {
			log_error("Aborting. Failed to wipe VDO data store %s.",
				  display_lvname(lv));
			return NULL;
		}
	}

	if (!convert_vdo_pool_lv(lv, &vcp->vdo_params, &lvc.virtual_extents, vcp->do_zero, vcp->header_size))
		return_NULL;

        /* Create VDO LV with the name, we just release above */
	if (!(vdo_lv = lv_create_single(lv->vg, &lvc)))
		return_NULL;

	if (vcp->lv_name)
		return vdo_lv;

	/* Swap vdo_lv and lv segment, so passed-in LV appears as virtual VDO_LV */
	if (!move_lv_segments(&tmp_lv, lv, 0, 0) ||
	    !move_lv_segments(lv, vdo_lv, 0, 0) ||
	    !move_lv_segments(vdo_lv, &tmp_lv, 0, 0))
		return_NULL;

	/* Also swap naming, so the passed in LV keeps the passed-in name */
	vdo_lv->name = lv->name;
	lv->name = lvc.lv_name;

	/* Swap segment referencing */
	if (!remove_seg_from_segs_using_this_lv(lv, first_seg(lv)))
		return_NULL;

	if (!set_lv_segment_area_lv(first_seg(lv), 0, vdo_lv, 0, 0))
		return_NULL;

	return lv;
}

int set_vdo_write_policy(enum dm_vdo_write_policy *vwp, const char *policy)
{
	if (strcasecmp(policy, "sync") == 0)
		*vwp = DM_VDO_WRITE_POLICY_SYNC;
	else if (strcasecmp(policy, "async") == 0)
		*vwp = DM_VDO_WRITE_POLICY_ASYNC;
	else if (strcasecmp(policy, "async-unsafe") == 0)
		*vwp = DM_VDO_WRITE_POLICY_ASYNC_UNSAFE;
	else if (strcasecmp(policy, "auto") == 0)
		*vwp = DM_VDO_WRITE_POLICY_AUTO;
	else {
		log_error("Unknown VDO write policy %s.", policy);
		return 0;
	}

	if (*vwp != DM_VDO_WRITE_POLICY_AUTO)
		log_info("Deprecated VDO setting write_policy specified.");

	return 1;
}

int fill_vdo_target_params(struct cmd_context *cmd,
			   struct dm_vdo_target_params *vtp,
			   uint64_t *vdo_pool_header_size,
			   struct profile *profile)
{
	const char *policy;

	// TODO: Postpone filling data to the moment when VG is known with profile.
	// TODO: Maybe add more lvm cmdline switches to set profile settings.

	vtp->use_compression =
		find_config_tree_int(cmd, allocation_vdo_use_compression_CFG, profile);
	vtp->use_deduplication =
		find_config_tree_int(cmd, allocation_vdo_use_deduplication_CFG, profile);
	vtp->use_metadata_hints =
		find_config_tree_int(cmd, allocation_vdo_use_metadata_hints_CFG, profile);
	vtp->minimum_io_size =
		find_config_tree_int(cmd, allocation_vdo_minimum_io_size_CFG, profile) >> SECTOR_SHIFT;
	vtp->block_map_cache_size_mb =
		find_config_tree_int64(cmd, allocation_vdo_block_map_cache_size_mb_CFG, profile);
	vtp->block_map_era_length =
		find_config_tree_int(cmd, allocation_vdo_block_map_era_length_CFG, profile);
	vtp->use_sparse_index =
		find_config_tree_int(cmd, allocation_vdo_use_sparse_index_CFG, profile);
	vtp->index_memory_size_mb =
		find_config_tree_int64(cmd, allocation_vdo_index_memory_size_mb_CFG, profile);
	vtp->slab_size_mb =
		find_config_tree_int(cmd, allocation_vdo_slab_size_mb_CFG, profile);
	vtp->ack_threads =
		find_config_tree_int(cmd, allocation_vdo_ack_threads_CFG, profile);
	vtp->bio_threads =
		find_config_tree_int(cmd, allocation_vdo_bio_threads_CFG, profile);
	vtp->bio_rotation =
		find_config_tree_int(cmd, allocation_vdo_bio_rotation_CFG, profile);
	vtp->cpu_threads =
		find_config_tree_int(cmd, allocation_vdo_cpu_threads_CFG, profile);
	vtp->hash_zone_threads =
		find_config_tree_int(cmd, allocation_vdo_hash_zone_threads_CFG, profile);
	vtp->logical_threads =
		find_config_tree_int(cmd, allocation_vdo_logical_threads_CFG, profile);
	vtp->physical_threads =
		find_config_tree_int(cmd, allocation_vdo_physical_threads_CFG, profile);
	vtp->max_discard =
		find_config_tree_int(cmd, allocation_vdo_max_discard_CFG, profile);

	policy = find_config_tree_str(cmd, allocation_vdo_write_policy_CFG, profile);
	if (!set_vdo_write_policy(&vtp->write_policy, policy))
		return_0;

	*vdo_pool_header_size = 2 * find_config_tree_int64(cmd, allocation_vdo_pool_header_size_CFG, profile);

	if (vtp->use_metadata_hints)
		log_info("Deprecated VDO setting use_metadata_hints specified.");

	return 1;
}

static int _get_sysinfo_memory(uint64_t *total_mb, uint64_t *available_mb)
{
	struct sysinfo si = { 0 };

	*total_mb = *available_mb = UINT64_MAX;

	if (sysinfo(&si) != 0)
		return 0;

	log_debug("Sysinfo free:%llu  bufferram:%llu  sharedram:%llu  freehigh:%llu  unit:%u.",
		  (unsigned long long)si.freeram >> 20, (unsigned long long)si.bufferram >> 20, (unsigned long long)si.sharedram >> 20,
		  (unsigned long long)si.freehigh >> 20, si.mem_unit);

	*available_mb = ((uint64_t)(si.freeram + si.bufferram) * si.mem_unit) >> 30;
	*total_mb = si.totalram >> 30;

	return 1;
}

typedef struct mem_table_s {
	const char *name;
	uint64_t *value;
} mem_table_t;

static int _compare_mem_table_s(const void *a, const void *b){
	return strcmp(((const mem_table_t*)a)->name, ((const mem_table_t*)b)->name);
}

static int _get_memory_info(struct cmd_context *cmd, uint64_t *total_mb, uint64_t *available_mb)
{
	uint64_t anon_pages = 0, mem_available = 0, mem_free = 0, mem_total = 0, shmem = 0, swap_free = 0;
	uint64_t can_swap;
	mem_table_t mt[] = {
		{ "AnonPages",    &anon_pages },
		{ "MemAvailable", &mem_available },
		{ "MemFree",	  &mem_free },
		{ "MemTotal",	  &mem_total },
		{ "Shmem",	  &shmem },
		{ "SwapFree",	  &swap_free },
	};

	char line[128], namebuf[32], *e, *tail;
	char proc_meminfo[PATH_MAX];
	FILE *fp;
	mem_table_t findme = { namebuf, NULL };
	mem_table_t *found;

	if ((dm_snprintf(proc_meminfo, sizeof(proc_meminfo),
			 "%s/meminfo", cmd->proc_dir) < 0) ||
	    !(fp = fopen(proc_meminfo, "r")))
		return _get_sysinfo_memory(total_mb, available_mb);

	while (fgets(line, sizeof(line), fp)) {
		if (!(e = strchr(line, ':')))
			break;

		if ((unsigned)(++e - line) > sizeof(namebuf))
			continue; // something too long

		dm_strncpy((char*)findme.name, line, e - line);

		found = bsearch(&findme, mt, DM_ARRAY_SIZE(mt), sizeof(mem_table_t),
				_compare_mem_table_s);
		if (!found)
			continue; // not interesting

		errno = 0;
		*(found->value) = (uint64_t) strtoull(e, &tail, 10);

		if ((e == tail) || errno)
			log_debug("Failing to parse value from %s.", line);
		else
			log_debug("Parsed %s = " FMTu64 " KiB.", found->name, *(found->value));
	}
	(void)fclose(fp);

	// use at most 2/3 of swap space to keep machine usable
	can_swap = (anon_pages + shmem) * 2 / 3;
	swap_free = swap_free * 2 / 3;

	if (can_swap > swap_free)
		can_swap = swap_free;

	// TODO: add more constrains, i.e. 3/4 of physical RAM...

	*total_mb = mem_total >> 10;
	*available_mb = (mem_available + can_swap) >> 10;

	return 1;
}

static uint64_t _round_1024(uint64_t s)
{
	return (s + ((1 << 10) - 1)) >> 10;
}

static uint64_t _round_sectors_to_tib(uint64_t s)
{
	return  (s + ((UINT64_C(1) << (40 - SECTOR_SHIFT)) - 1)) >> (40 - SECTOR_SHIFT);
}

__attribute__ ((format(printf, 3, 4)))
static int _vdo_snprintf(char **buf, size_t *bufsize, const char *format, ...)
{
	int n;
	va_list ap;

	va_start(ap, format);
	n = vsnprintf(*buf, *bufsize, format, ap);
	va_end(ap);

	if (n < 0 || ((unsigned) n >= *bufsize))
		return -1;

	*buf += n;
	*bufsize -= n;

	return n;
}

int check_vdo_constrains(struct cmd_context *cmd, const struct vdo_pool_size_config *cfg)
{
	static const char _vdo_split[][4] = { "", " and", ",", "," };
	uint64_t req_mb, total_mb, available_mb;
	uint64_t phy_mb = _round_sectors_to_tib(UINT64_C(268) * cfg->physical_size); // 268 MiB per 1 TiB of physical size
	uint64_t virt_mb = _round_1024(UINT64_C(1638) * _round_sectors_to_tib(cfg->virtual_size)); // 1.6 MiB per 1 TiB
	uint64_t cache_mb = _round_1024(UINT64_C(1177) * cfg->block_map_cache_size_mb); // 1.15 MiB per 1 MiB cache size
	char msg[512];
	size_t mlen = sizeof(msg);
	char *pmsg = msg;
	int cnt, has_cnt;

	if (cfg->block_map_cache_size_mb && (cache_mb < 150))
		cache_mb = 150; // always at least 150 MiB for block map

	// total required memory for VDO target
	req_mb = 38 + cfg->index_memory_size_mb + virt_mb + phy_mb + cache_mb;

	_get_memory_info(cmd, &total_mb, &available_mb);

	has_cnt = cnt = (phy_mb ? 1 : 0) +
			 (virt_mb ? 1 : 0) +
			 (cfg->block_map_cache_size_mb ? 1 : 0) +
			 (cfg->index_memory_size_mb ? 1 : 0);

	if (phy_mb)
		(void)_vdo_snprintf(&pmsg, &mlen, " %s RAM for physical volume size %s%s",
				    display_size(cmd, phy_mb << (20 - SECTOR_SHIFT)),
				    display_size(cmd, cfg->physical_size), _vdo_split[--cnt]);

	if (virt_mb)
		(void)_vdo_snprintf(&pmsg, &mlen, " %s RAM for virtual volume size %s%s",
				    display_size(cmd, virt_mb << (20 - SECTOR_SHIFT)),
				    display_size(cmd, cfg->virtual_size), _vdo_split[--cnt]);

	if (cfg->block_map_cache_size_mb)
		(void)_vdo_snprintf(&pmsg, &mlen, " %s RAM for block map cache size %s%s",
				    display_size(cmd, cache_mb << (20 - SECTOR_SHIFT)),
				    display_size(cmd, ((uint64_t)cfg->block_map_cache_size_mb) << (20 - SECTOR_SHIFT)),
				    _vdo_split[--cnt]);

	if (cfg->index_memory_size_mb)
		(void)_vdo_snprintf(&pmsg, &mlen, " %s RAM for index memory",
				    display_size(cmd, ((uint64_t)cfg->index_memory_size_mb) << (20 - SECTOR_SHIFT)));

	if (req_mb > available_mb) {
		log_error("Not enough free memory for VDO target. %s RAM is required, but only %s RAM is available.",
			  display_size(cmd, req_mb << (20 - SECTOR_SHIFT)),
			  display_size(cmd, available_mb << (20 - SECTOR_SHIFT)));
		if (has_cnt)
			log_print_unless_silent("VDO configuration needs%s.", msg);
		return 0;
	}

	log_debug("VDO requires %s RAM, currently available %s RAM.",
		  display_size(cmd, req_mb << (20 - SECTOR_SHIFT)),
		  display_size(cmd, available_mb << (20 - SECTOR_SHIFT)));

	if (has_cnt)
		log_verbose("VDO configuration needs%s.", msg);

	return 1;
}
