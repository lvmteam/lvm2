/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Luca Berra
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_FILTER_H
#define _LVM_FILTER_H

#include "lib/device/dev-cache.h"
#include "lib/device/dev-type.h"

struct dev_filter *composite_filter_create(int n, struct dev_filter **filters);

struct dev_filter *lvm_type_filter_create(struct dev_types *dt);
struct dev_filter *md_filter_create(struct cmd_context *cmd, struct dev_types *dt);
struct dev_filter *fwraid_filter_create(struct dev_types *dt);
struct dev_filter *mpath_filter_create(struct dev_types *dt);
struct dev_filter *partitioned_filter_create(struct dev_types *dt);
struct dev_filter *persistent_filter_create(struct dev_types *dt, struct dev_filter *f);
struct dev_filter *sysfs_filter_create(void);
struct dev_filter *signature_filter_create(struct dev_types *dt);
struct dev_filter *deviceid_filter_create(struct cmd_context *cmd);

/*
 * patterns must be an array of strings of the form:
 * [ra]<sep><regex><sep>, eg,
 * r/cdrom/          - reject cdroms
 * a|loop/[0-4]|     - accept loops 0 to 4
 * r|.*|             - reject everything else
 */

struct dev_filter *regex_filter_create(const struct dm_config_value *patterns, int config_filter, int config_global_filter);

typedef enum {
	FILTER_MODE_NO_LVMETAD,
	FILTER_MODE_PRE_LVMETAD,
	FILTER_MODE_POST_LVMETAD
} filter_mode_t;
struct dev_filter *usable_filter_create(struct cmd_context *cmd, struct dev_types *dt, filter_mode_t mode);

#define DEV_FILTERED_FWRAID		0x00000001
#define DEV_FILTERED_INTERNAL		0x00000002
#define DEV_FILTERED_MD_COMPONENT	0x00000004
#define DEV_FILTERED_MPATH_COMPONENT	0x00000008
#define DEV_FILTERED_PARTITIONED	0x00000010
#define DEV_FILTERED_REGEX		0x00000020
#define DEV_FILTERED_SIGNATURE		0x00000040
#define DEV_FILTERED_SYSFS		0x00000080
#define DEV_FILTERED_DEVTYPE		0x00000100
#define DEV_FILTERED_MINSIZE		0x00000200
#define DEV_FILTERED_UNUSABLE		0x00000400
#define DEV_FILTERED_DEVICES_FILE	0x00000800
#define DEV_FILTERED_DEVICES_LIST	0x00001000
#define DEV_FILTERED_IS_LV		0x00002000

#endif 	/* _LVM_FILTER_H */
