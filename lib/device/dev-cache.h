/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_DEV_CACHE_H
#define _LVM_DEV_CACHE_H

#include "lib/device/device.h"
#include "lib/device/dev-type.h"
#include "lib/misc/lvm-wrappers.h"

struct cmd_context;

/*
 * predicate for devices.
 */
struct dev_filter {
	int (*passes_filter) (struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name);
	void (*destroy) (struct dev_filter *f);
	void (*wipe) (struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name);
	void *private;
	unsigned use_count;
	const char *name;
};

void dev_init(struct device *dev);

struct dm_list *dev_cache_get_dev_list_for_vgid(struct cmd_context *cmd, const char *vgid);
struct dm_list *dev_cache_get_dev_list_for_lvid(struct cmd_context *cmd, const char *lvid);

/*
 * The cache of dm devices is enabled when the kernel
 * supports the ability to quickly report on many dm
 * devs together, in which case we can get all the dm
 * info at once and store it in this dm_devs_cache.
 * This avoids many individual dm dev ioctl calls.
 * The callers of these dm_devs_cache functions must
 * have an alternative for when dm_devs_cache_use()
 * returns 0.
 */
int dm_devs_cache_use(struct cmd_context *cmd);
int dm_devs_cache_update(struct cmd_context *cmd);
void dm_devs_cache_destroy(struct cmd_context *cmd);
void dm_devs_cache_label_invalidate(struct cmd_context *cmd);
const struct dm_active_device *
dm_devs_cache_get_by_devno(struct cmd_context *cmd, dev_t devno);
const struct dm_active_device *
dm_devs_cache_get_by_uuid(struct cmd_context *cmd, const char *dm_uuid);

/*
 * The global device cache.
 */
int dev_cache_init(struct cmd_context *cmd);
int dev_cache_exit(struct cmd_context *cmd);

void dev_cache_scan(struct cmd_context *cmd);

int dev_cache_add_dir(struct cmd_context *cmd, const char *path);
struct device *dev_cache_get(struct cmd_context *cmd, const char *name, struct dev_filter *f);
struct device *dev_cache_get_existing(struct cmd_context *cmd, const char *name, struct dev_filter *f);
void dev_cache_verify_aliases(struct cmd_context *cmd, struct device *dev);

struct device *dev_cache_get_by_devno(struct cmd_context *cmd, dev_t devno);
struct device *dev_cache_get_by_name(struct cmd_context *cmd, const char *name);
struct device *dev_cache_get_by_pvid(struct cmd_context *cmd, const char *pvid);

void dev_set_preferred_name(struct cmd_context *cmd, struct dm_str_list *sl, struct device *dev);

/*
 * Object for iterating through the cache.
 */
struct dev_iter;
struct dev_iter *dev_iter_create(struct cmd_context *cmd, struct dev_filter *f, int unused);
void dev_iter_destroy(struct dev_iter *iter);
struct device *dev_iter_get(struct cmd_context *cmd, struct dev_iter *iter);

void dev_cache_failed_path(struct cmd_context *cmd, struct device *dev, const char *path);

bool dev_cache_has_md_with_end_superblock(struct cmd_context *cmd, struct dev_types *dt);

int get_sysfs_value(const char *path, char *buf, size_t buf_size, int error_if_no_value);
int get_sysfs_binary(const char *path, char *buf, size_t buf_size, int *retlen);

int setup_devices_file(struct cmd_context *cmd);
int setup_devices(struct cmd_context *cmd);
int setup_device(struct cmd_context *cmd, const char *devname);

int setup_devices_for_online_autoactivation(struct cmd_context *cmd);
int setup_devname_in_dev_cache(struct cmd_context *cmd, const char *devname);
int setup_devno_in_dev_cache(struct cmd_context *cmd, dev_t devno);
struct device *setup_dev_in_dev_cache(struct cmd_context *cmd, dev_t devno, const char *devname);

#endif
