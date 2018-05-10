/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
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

#ifndef DEVICE_MAPPER_VDO_TARGET_H
#define DEVICE_MAPPER_VDO_TARGET_H

#include <stdbool.h>
#include <stdint.h>

//----------------------------------------------------------------

enum vdo_operating_mode {
	VDO_MODE_RECOVERING,
	VDO_MODE_READ_ONLY,
	VDO_MODE_NORMAL
};

enum vdo_compression_state {
	VDO_COMPRESSION_ONLINE,
	VDO_COMPRESSION_OFFLINE
};

enum vdo_index_state {
	VDO_INDEX_ERROR,
	VDO_INDEX_CLOSED,
	VDO_INDEX_OPENING,
	VDO_INDEX_CLOSING,
	VDO_INDEX_OFFLINE,
	VDO_INDEX_ONLINE,
	VDO_INDEX_UNKNOWN
};

struct vdo_status {
	char *device;
	enum vdo_operating_mode operating_mode;
	bool recovering;
	enum vdo_index_state index_state;
	enum vdo_compression_state compression_state;
	uint64_t used_blocks;
	uint64_t total_blocks;
};

void vdo_status_destroy(struct vdo_status *s);

#define VDO_MAX_ERROR 256

struct vdo_status_parse_result {
	char error[VDO_MAX_ERROR];
	struct vdo_status *status;
};

// Parses the status line from the kernel target.
bool vdo_status_parse(const char *input, struct vdo_status_parse_result *result);

//----------------------------------------------------------------

#endif
