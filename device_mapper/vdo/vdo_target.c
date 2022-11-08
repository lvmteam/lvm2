/*
 * Copyright (C) 2018-2022 Red Hat, Inc. All rights reserved.
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

#include "device_mapper/misc/dmlib.h"
#include "device_mapper/all.h"

#include "vdo_limits.h"
#include "target.h"

/* validate vdo target parameters and  'vdo_size' in sectors */
bool dm_vdo_validate_target_params(const struct dm_vdo_target_params *vtp,
				   uint64_t vdo_size)
{
	bool valid = true;

	/* 512 or 4096 bytes only ATM */
	if ((vtp->minimum_io_size != (512 >> SECTOR_SHIFT)) &&
	    (vtp->minimum_io_size != (4096 >> SECTOR_SHIFT))) {
		log_error("VDO minimum io size %u is unsupported [512, 4096].",
			  vtp->minimum_io_size);
		valid = false;
	}

	if ((vtp->block_map_cache_size_mb < DM_VDO_BLOCK_MAP_CACHE_SIZE_MINIMUM_MB) ||
	    (vtp->block_map_cache_size_mb > DM_VDO_BLOCK_MAP_CACHE_SIZE_MAXIMUM_MB)) {
		log_error("VDO block map cache size %u MiB is out of range [%u..%u].",
			  vtp->block_map_cache_size_mb,
			  DM_VDO_BLOCK_MAP_CACHE_SIZE_MINIMUM_MB,
			  DM_VDO_BLOCK_MAP_CACHE_SIZE_MAXIMUM_MB);
		valid = false;
	}

	if ((vtp->block_map_era_length < DM_VDO_BLOCK_MAP_ERA_LENGTH_MINIMUM) ||
	    (vtp->block_map_era_length > DM_VDO_BLOCK_MAP_ERA_LENGTH_MAXIMUM)) {
		log_error("VDO block map era length %u is out of range [%u..%u].",
			  vtp->block_map_era_length,
			  DM_VDO_BLOCK_MAP_ERA_LENGTH_MINIMUM,
			  DM_VDO_BLOCK_MAP_ERA_LENGTH_MAXIMUM);
		valid = false;
	}

	if ((vtp->index_memory_size_mb < DM_VDO_INDEX_MEMORY_SIZE_MINIMUM_MB) ||
	    (vtp->index_memory_size_mb > DM_VDO_INDEX_MEMORY_SIZE_MAXIMUM_MB)) {
		log_error("VDO index memory size %u MiB is out of range [%u..%u].",
			  vtp->index_memory_size_mb,
			  DM_VDO_INDEX_MEMORY_SIZE_MINIMUM_MB,
			  DM_VDO_INDEX_MEMORY_SIZE_MAXIMUM_MB);
		valid = false;
	}

	if ((vtp->slab_size_mb < DM_VDO_SLAB_SIZE_MINIMUM_MB) ||
	    (vtp->slab_size_mb > DM_VDO_SLAB_SIZE_MAXIMUM_MB)) {
		log_error("VDO slab size %u MiB is out of range [%u..%u].",
			  vtp->slab_size_mb,
			  DM_VDO_SLAB_SIZE_MINIMUM_MB,
			  DM_VDO_SLAB_SIZE_MAXIMUM_MB);
		valid = false;
	}

	if ((vtp->max_discard < DM_VDO_MAX_DISCARD_MINIMUM) ||
	    (vtp->max_discard > DM_VDO_MAX_DISCARD_MAXIMUM)) {
		log_error("VDO max discard %u is out of range [%u..%u].",
			  vtp->max_discard,
			  DM_VDO_MAX_DISCARD_MINIMUM,
			  DM_VDO_MAX_DISCARD_MAXIMUM);
		valid = false;
	}

	if (vtp->ack_threads > DM_VDO_ACK_THREADS_MAXIMUM) {
		log_error("VDO ack threads %u is out of range [0..%u].",
			  vtp->ack_threads,
			  DM_VDO_ACK_THREADS_MAXIMUM);
		valid = false;
	}

	if ((vtp->bio_threads < DM_VDO_BIO_THREADS_MINIMUM) ||
	    (vtp->bio_threads > DM_VDO_BIO_THREADS_MAXIMUM)) {
		log_error("VDO bio threads %u is out of range [%u..%u].",
			  vtp->bio_threads,
			  DM_VDO_BIO_THREADS_MINIMUM,
			  DM_VDO_BIO_THREADS_MAXIMUM);
		valid = false;
	}

	if ((vtp->bio_rotation < DM_VDO_BIO_ROTATION_MINIMUM) ||
	    (vtp->bio_rotation > DM_VDO_BIO_ROTATION_MAXIMUM)) {
		log_error("VDO bio rotation %u is out of range [%u..%u].",
			  vtp->bio_rotation,
			  DM_VDO_BIO_ROTATION_MINIMUM,
			  DM_VDO_BIO_ROTATION_MAXIMUM);
		valid = false;
	}

	if ((vtp->cpu_threads < DM_VDO_CPU_THREADS_MINIMUM) ||
	    (vtp->cpu_threads > DM_VDO_CPU_THREADS_MAXIMUM)) {
		log_error("VDO cpu threads %u is out of range [%u..%u].",
			  vtp->cpu_threads,
			  DM_VDO_CPU_THREADS_MINIMUM,
			  DM_VDO_CPU_THREADS_MAXIMUM);
		valid = false;
	}

	if (vtp->hash_zone_threads > DM_VDO_HASH_ZONE_THREADS_MAXIMUM) {
		log_error("VDO hash zone threads %u is out of range [0..%u].",
			  vtp->hash_zone_threads,
			  DM_VDO_HASH_ZONE_THREADS_MAXIMUM);
		valid = false;
	}

	if (vtp->logical_threads > DM_VDO_LOGICAL_THREADS_MAXIMUM) {
		log_error("VDO logical threads %u is out of range [0..%u].",
			  vtp->logical_threads,
			  DM_VDO_LOGICAL_THREADS_MAXIMUM);
		valid = false;
	}

	if (vtp->physical_threads > DM_VDO_PHYSICAL_THREADS_MAXIMUM) {
		log_error("VDO physical threads %u is out of range [0..%u].",
			  vtp->physical_threads,
			  DM_VDO_PHYSICAL_THREADS_MAXIMUM);
		valid = false;
	}

	switch (vtp->write_policy) {
	case DM_VDO_WRITE_POLICY_SYNC:
	case DM_VDO_WRITE_POLICY_ASYNC:
	case DM_VDO_WRITE_POLICY_ASYNC_UNSAFE:
	case DM_VDO_WRITE_POLICY_AUTO:
		break;
	default:
		log_error(INTERNAL_ERROR "VDO write policy %u is unknown.", vtp->write_policy);
		valid = false;
	}

	if ((vtp->hash_zone_threads ||
	     vtp->logical_threads ||
	     vtp->physical_threads) &&
	    (!vtp->hash_zone_threads ||
	     !vtp->logical_threads ||
	     !vtp->physical_threads)) {
		log_error("Value of vdo_hash_zone_threads(%u), vdo_logical_threads(%u), "
			  "vdo_physical_threads(%u) must be all zero or all non-zero.",
			  vtp->hash_zone_threads, vtp->logical_threads, vtp->physical_threads);
		valid = false;
	}

	if (vdo_size > DM_VDO_LOGICAL_SIZE_MAXIMUM) {
		log_error("VDO logical size is larger than limit " FMTu64 " TiB by " FMTu64 " KiB.",
			  DM_VDO_LOGICAL_SIZE_MAXIMUM / (UINT64_C(1024) * 1024 * 1024 * 1024 >> SECTOR_SHIFT),
			  (vdo_size - DM_VDO_LOGICAL_SIZE_MAXIMUM) / 2);
		valid = false;
	}

	return valid;
}
