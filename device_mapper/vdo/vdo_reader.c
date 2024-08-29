/*
 * Copyright (C) 2022 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Based on VDO sources: https://github.com/dm-vdo/vdo
 *
 * Simplified parser of VDO superblock to obtain basic VDO parameteers
 *
 * TODO: maybe switch to some library in the future
 */

//#define _GNU_SOURCE 1
//#define _LARGEFILE64_SOURCE 1

#include "device_mapper/misc/dmlib.h"

#include "target.h"

#include "lib/mm/xlate.h"
//#include "linux/byteorder/big_endian.h"
//#include "linux/byteorder/little_endian.h"
//#define le32_to_cpu __le32_to_cpu
//#define le64_to_cpu __le64_to_cpu


#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>	/* For block ioctl definitions */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef unsigned char uuid_t[16];

#define __packed	__attribute__((packed))

static const char _MAGIC_NUMBER[] = "dmvdo001";
#define MAGIC_NUMBER_SIZE  (sizeof(_MAGIC_NUMBER) - 1)

struct vdo_version_number {
	uint32_t major_version;
	uint32_t minor_version;
} __packed;

/*
 * The registry of component ids for use in headers
 */
enum {
	SUPER_BLOCK	  = 0,
	FIXED_LAYOUT	  = 1,
	RECOVERY_JOURNAL  = 2,
	SLAB_DEPOT	  = 3,
	BLOCK_MAP	  = 4,
	GEOMETRY_BLOCK	  = 5,
}; /* ComponentID */

struct vdo_header {
	uint32_t id; /* The component this is a header for */
	struct vdo_version_number version; /* The version of the data format */
	size_t size; /* The size of the data following this header */
} __packed;

struct vdo_geometry_block {
	char magic_number[MAGIC_NUMBER_SIZE];
	struct vdo_header header;
	uint32_t checksum;
} __packed;

struct vdo_config {
	uint64_t logical_blocks; /* number of logical blocks */
	uint64_t physical_blocks; /* number of physical blocks */
	uint64_t slab_size; /* number of blocks in a slab */
	uint64_t recovery_journal_size; /* number of recovery journal blocks */
	uint64_t slab_journal_blocks; /* number of slab journal blocks */
} __packed;

struct vdo_component_41_0 {
	uint32_t state;
	uint64_t complete_recoveries;
	uint64_t read_only_recoveries;
	struct vdo_config config; /* packed */
	uint64_t nonce;
} __packed;

enum vdo_volume_region_id {
	VDO_INDEX_REGION = 0,
	VDO_DATA_REGION = 1,
	VDO_VOLUME_REGION_COUNT,
};

struct vdo_volume_region {
	/* The ID of the region */
	enum vdo_volume_region_id id;
	/*
	 * The absolute starting offset on the device. The region continues
	 * until the next region begins.
	 */
	uint64_t start_block;
} __packed;

struct vdo_index_config {
	uint32_t mem;
	uint32_t unused;
	uint8_t sparse;
} __packed;

struct vdo_volume_geometry {
	uint32_t release_version;
	uint64_t nonce;
	uuid_t uuid;
	uint64_t bio_offset;
	struct vdo_volume_region regions[VDO_VOLUME_REGION_COUNT];
	struct vdo_index_config index_config;
} __packed;

struct vdo_volume_geometry_4 {
	uint32_t release_version;
	uint64_t nonce;
	uuid_t uuid;
	struct vdo_volume_region regions[VDO_VOLUME_REGION_COUNT];
	struct vdo_index_config index_config;
} __packed;

/* Decoding mostly only some used structure members */

static void _vdo_decode_version(struct vdo_version_number *v)
{
	v->major_version = le32_to_cpu(v->major_version);
	v->minor_version = le32_to_cpu(v->minor_version);
}

static void _vdo_decode_header(struct vdo_header *h)
{
	h->id = le32_to_cpu(h->id);
	_vdo_decode_version(&h->version);
	h->size = le64_to_cpu(h->size);
}

static void _vdo_decode_geometry_region(struct vdo_volume_region *vr)
{
	vr->id = le32_to_cpu(vr->id);
	vr->start_block = le64_to_cpu(vr->start_block);
}

static void _vdo_decode_volume_geometry(struct vdo_volume_geometry *vg)
{
	vg->release_version = le32_to_cpu(vg->release_version);
	vg->nonce = le64_to_cpu(vg->nonce);
	vg->bio_offset = le64_to_cpu(vg->bio_offset);
	_vdo_decode_geometry_region(&vg->regions[VDO_DATA_REGION]);
}

static void _vdo_decode_volume_geometry_4(struct vdo_volume_geometry *vg,
					  struct vdo_volume_geometry_4 *vg_4)
{
	vg->release_version = le32_to_cpu(vg_4->release_version);
	vg->nonce = le64_to_cpu(vg_4->nonce);
	vg->bio_offset = 0;
	vg->regions[VDO_DATA_REGION] = vg_4->regions[VDO_DATA_REGION];
	_vdo_decode_geometry_region(&vg->regions[VDO_DATA_REGION]);
}

static void _vdo_decode_config(struct vdo_config *vc)
{
	vc->logical_blocks = le64_to_cpu(vc->logical_blocks);
	vc->physical_blocks = le64_to_cpu(vc->physical_blocks);
	vc->slab_size = le64_to_cpu(vc->slab_size);
	vc->recovery_journal_size = le64_to_cpu(vc->recovery_journal_size);
	vc->slab_journal_blocks = le64_to_cpu(vc->slab_journal_blocks);
}

static void _vdo_decode_pvc(struct vdo_component_41_0 *pvc)
{
	_vdo_decode_config(&pvc->config);
	pvc->nonce = le64_to_cpu(pvc->nonce);
}

bool dm_vdo_parse_logical_size(const char *vdo_path, uint64_t *logical_blocks)
{
	char buffer[4096];
	int fh;
	bool r = false;
	struct stat st;
	uint64_t size;
	uint64_t regpos;

	struct vdo_header h;
	struct vdo_version_number vn;
	struct vdo_volume_geometry vg;
	struct vdo_volume_geometry_4 vg_4;
	struct vdo_component_41_0 pvc;

	*logical_blocks = 0;
	if ((fh = open(vdo_path, O_RDONLY)) == -1) {
		log_sys_debug("Failed to open VDO backend %s.", vdo_path);
		return false;
	}

	if (ioctl(fh, BLKGETSIZE64, &size) == -1) {
		if (errno != ENOTTY) {
			log_sys_debug("ioctl", vdo_path);
			goto err;
		}

		/* lets retry for file sizes */
		if (fstat(fh, &st) < 0) {
			log_sys_debug("fstat", vdo_path);
			goto err;
		}

		size = st.st_size;
	}

	if (read(fh, buffer, sizeof(buffer)) < 0) {
		log_sys_debug("read", vdo_path);
		goto err;
	}

	if (strncmp(buffer, _MAGIC_NUMBER, MAGIC_NUMBER_SIZE)) {
		log_debug_activation("Found mismatching VDO magic header in %s.", vdo_path);
		goto err;
	}

	memcpy(&h, buffer + MAGIC_NUMBER_SIZE, sizeof(h));
	_vdo_decode_header(&h);

	if (h.id != 5) {
		log_debug_activation("Expected geometry VDO block instead of block %u.", h.id);
		goto err;
	}

	switch (h.version.major_version) {
	case 4:
		memcpy(&vg_4, buffer + MAGIC_NUMBER_SIZE + sizeof(h), sizeof(vg_4));
		_vdo_decode_volume_geometry_4(&vg, &vg_4);
		break;
	case 5:
		memcpy(&vg, buffer + MAGIC_NUMBER_SIZE + sizeof(h), sizeof(vg));
		_vdo_decode_volume_geometry(&vg);
		break;
	default:
		log_debug_activation("Unsupported VDO version %u.%u.", h.version.major_version, h.version.minor_version);
		goto err;
	}

	regpos = (vg.regions[VDO_DATA_REGION].start_block - vg.bio_offset) * 4096;

	if ((regpos + sizeof(buffer)) > size) {
		log_debug_activation("File/Device is shorter and can't provide requested VDO volume region at " FMTu64 " > " FMTu64 ".", regpos, size);
		goto err;
	}

	if (lseek(fh, regpos, SEEK_SET) < 0) {
		log_sys_debug("lseek", vdo_path);
		goto err;
	}

	if (read(fh, buffer, sizeof(buffer)) < 0) {
		log_sys_debug("read", vdo_path);
		goto err;
	}

	memcpy(&vn, buffer + sizeof(struct vdo_geometry_block), sizeof(vn));
	_vdo_decode_version(&vn);

	if (vn.major_version > 41) {
		log_debug_activation("Unknown VDO component version %u.", vn.major_version); // should be 41!
		goto err;
	}

	memcpy(&pvc, buffer + sizeof(struct vdo_geometry_block) + sizeof(vn), sizeof(pvc));
	_vdo_decode_pvc(&pvc);

	if (pvc.nonce != vg.nonce) {
		log_debug_activation("VDO metadata has mismatching VDO nonces " FMTu64 " != " FMTu64 ".", pvc.nonce, vg.nonce);
		goto err;
	}

#if 0
	log_debug_activation("LogBlocks    " FMTu64 ".", pvc.config.logical_blocks);
	log_debug_activation("PhyBlocks    " FMTu64 ".", pvc.config.physical_blocks);
	log_debug_activation("SlabSize     " FMTu64 ".", pvc.config.slab_size);
	log_debug_activation("RecJourSize  " FMTu64 ".", pvc.config.recovery_journal_size);
	log_debug_activation("SlabJouSize  " FMTu64 ".", pvc.config.slab_journal_blocks);
#endif

	*logical_blocks = pvc.config.logical_blocks;
	r = true;
err:
	(void) close(fh);

	return r;
}
