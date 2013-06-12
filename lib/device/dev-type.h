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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_DEV_TYPE_H
#define _LVM_DEV_TYPE_H

#include "device.h"

#define NUMBER_OF_MAJORS 4096

#ifdef linux
#  define MAJOR(dev)	((dev & 0xfff00) >> 8)
#  define MINOR(dev)	((dev & 0xff) | ((dev >> 12) & 0xfff00))
#  define MKDEV(ma,mi)	((mi & 0xff) | (ma << 8) | ((mi & ~0xff) << 12))
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
	int md_major;
	int blkext_major;
	int drbd_major;
	int device_mapper_major;
	int emcpower_major;
	int power2_major;
	struct dev_type_def dev_type_array[NUMBER_OF_MAJORS];
};

struct dev_types *create_dev_types(const char *proc_dir, const struct dm_config_node *cn);

/* Subsystems */
int dev_subsystem_part_major(struct dev_types *dt, struct device *dev);
const char *dev_subsystem_name(struct dev_types *dt, struct device *dev);
int major_is_scsi_device(struct dev_types *dt, int major);

/* Signature/superblock recognition with position returned where found. */
int dev_is_md(struct device *dev, uint64_t *sb);
int dev_is_swap(struct device *dev, uint64_t *signature);
int dev_is_luks(struct device *dev, uint64_t *signature);

/* Type-specific device properties */
unsigned long dev_md_stripe_width(struct dev_types *dt, struct device *dev);

/* Partitioning */
int major_max_partitions(struct dev_types *dt, int major);
int dev_is_partitioned(struct dev_types *dt, struct device *dev);
int dev_get_primary_dev(struct dev_types *dt, struct device *dev, dev_t *result);

/* Various device properties */
unsigned long dev_alignment_offset(struct dev_types *dt, struct device *dev);
unsigned long dev_minimum_io_size(struct dev_types *dt, struct device *dev);
unsigned long dev_optimal_io_size(struct dev_types *dt, struct device *dev);
unsigned long dev_discard_max_bytes(struct dev_types *dt, struct device *dev);
unsigned long dev_discard_granularity(struct dev_types *dt, struct device *dev);

#endif
