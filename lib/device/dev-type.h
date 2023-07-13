/*
 * Copyright (C) 2013 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_DEV_TYPE_H
#define _LVM_DEV_TYPE_H

#include "lib/device/device.h"
#include "lib/display/display.h"
#include "lib/label/label.h"
#include "lib/device/filesystem.h"

#define NUMBER_OF_MAJORS 4096

#ifdef __linux__
#  include "libdm/misc/kdev_t.h"
#else
#  define MAJOR(x) major((x))
#  define MINOR(x) minor((x))
#  define MKDEV(x,y) makedev((x),(y))
#endif

#define PARTITION_SCSI_DEVICE (1 << 0)

struct dev_type_def {
	int max_partitions; /* 0 means LVM won't use this major number. */
	int flags;
};

struct dev_types {
	unsigned md_major;
	unsigned blkext_major;
	unsigned drbd_major;
	unsigned device_mapper_major;
	unsigned emcpower_major;
	unsigned vxdmp_major;
	unsigned power2_major;
	unsigned dasd_major;
	unsigned loop_major;
	struct dev_type_def dev_type_array[NUMBER_OF_MAJORS];
};

struct dev_types *create_dev_types(const char *proc_dir, const struct dm_config_node *cn);

/* Subsystems */
int dev_subsystem_part_major(struct dev_types *dt, struct device *dev);
const char *dev_subsystem_name(struct dev_types *dt, struct device *dev);
int major_is_scsi_device(struct dev_types *dt, int major);

/* Signature/superblock recognition with position returned where found. */
int dev_is_md_component(struct cmd_context *cmd, struct device *dev, uint64_t *sb, int full);
int dev_is_mpath_component(struct cmd_context *cmd, struct device *dev, dev_t *mpath_devno);
int dev_is_swap(struct cmd_context *cmd, struct device *dev, uint64_t *signature, int full);
int dev_is_luks(struct cmd_context *cmd, struct device *dev, uint64_t *signature, int full);
int dasd_is_cdl_formatted(struct device *dev);

const char *dev_mpath_component_wwid(struct cmd_context *cmd, struct device *dev);

int dev_is_lvm1(struct device *dev, char *buf, int buflen);
int dev_is_pool(struct device *dev, char *buf, int buflen);

/* Signature wiping. */
#define TYPE_LVM1_MEMBER	0x001
#define TYPE_LVM2_MEMBER	0x002
#define TYPE_DM_SNAPSHOT_COW	0x004
int wipe_known_signatures(struct cmd_context *cmd, struct device *dev, const char *name,
			  uint32_t types_to_exclude, uint32_t types_no_prompt,
			  int yes, force_t force, int *wiped);

/* Type-specific device properties */
unsigned long dev_md_stripe_width(struct dev_types *dt, struct device *dev);
int dev_is_md_with_end_superblock(struct dev_types *dt, struct device *dev);

/* Partitioning */
int major_max_partitions(struct dev_types *dt, int major);
int dev_is_partitioned(struct cmd_context *cmd, struct device *dev);
int dev_get_primary_dev(struct dev_types *dt, struct device *dev, dev_t *result);
int dev_get_partition_number(struct device *dev, int *num);

/* Various device properties */
unsigned long dev_alignment_offset(struct dev_types *dt, struct device *dev);
unsigned long dev_minimum_io_size(struct dev_types *dt, struct device *dev);
unsigned long dev_optimal_io_size(struct dev_types *dt, struct device *dev);
unsigned long dev_discard_max_bytes(struct dev_types *dt, struct device *dev);
unsigned long dev_discard_granularity(struct dev_types *dt, struct device *dev);

int dev_is_rotational(struct dev_types *dt, struct device *dev);

int dev_is_pmem(struct dev_types *dt, struct device *dev);

int dev_is_nvme(struct dev_types *dt, struct device *dev);

int dev_is_lv(struct device *dev);

#define FSTYPE_MAX 16
int fs_block_size_and_type(const char *pathname, uint32_t *fs_block_size_bytes, char *fstype, int *nofs);
int fs_get_blkid(const char *pathname, struct fs_info *fsi);

int dev_is_used_by_active_lv(struct cmd_context *cmd, struct device *dev, int *used_by_lv_count,
			     char **used_by_dm_name, char **used_by_vg_uuid, char **used_by_lv_uuid);

#endif
