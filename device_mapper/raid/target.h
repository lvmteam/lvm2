/*
 * Copyright (C) 2024 Red Hat, Inc. All rights reserved.
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

#ifndef DEVICE_MAPPER_RAID_TARGET_H
#define DEVICE_MAPPER_RAID_TARGET_H

#include <stdint.h>

int dm_raid_count_failed_devices(const char *dev_path, uint32_t *nr_failed);
int dm_raid_clear_failed_devices(const char *dev_path, uint32_t *nr_failed);

#endif
