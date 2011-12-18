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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_CONFIG_H
#define _LVM_CONFIG_H

#include "lvm-types.h"

struct device;
struct cmd_context;

int override_config_tree_from_string(struct cmd_context *cmd,
				     const char *config_settings);
struct dm_config_tree *remove_overridden_config_tree(struct cmd_context *cmd);

typedef uint32_t (*checksum_fn_t) (uint32_t initial, const uint8_t *buf, uint32_t size);

struct dm_config_tree *config_file_open(const char *filename, int keep_open);
int config_file_read_fd(struct dm_config_tree *cft, struct device *dev,
			off_t offset, size_t size, off_t offset2, size_t size2,
			checksum_fn_t checksum_fn, uint32_t checksum);
	int config_file_read(struct dm_config_tree *cft);
int config_write(struct dm_config_tree *cft, const char *file,
		 int argc, char **argv);
void config_file_destroy(struct dm_config_tree *cft);

time_t config_file_timestamp(struct dm_config_tree *cft);
int config_file_changed(struct dm_config_tree *cft);
int config_file_check(struct dm_config_tree *cft, const char **filename, struct stat *info);


int merge_config_tree(struct cmd_context *cmd, struct dm_config_tree *cft,
		      struct dm_config_tree *newdata);

/*
 * These versions check an override tree, if present, first.
 */
const struct dm_config_node *find_config_tree_node(struct cmd_context *cmd,
						   const char *path);
const char *find_config_tree_str(struct cmd_context *cmd,
				 const char *path, const char *fail);
const char *find_config_tree_str_allow_empty(struct cmd_context *cmd,
					     const char *path, const char *fail);
int find_config_tree_int(struct cmd_context *cmd, const char *path,
			 int fail);
int64_t find_config_tree_int64(struct cmd_context *cmd, const char *path,
			     int64_t fail);
float find_config_tree_float(struct cmd_context *cmd, const char *path,
			     float fail);

int find_config_tree_bool(struct cmd_context *cmd, const char *path, int fail);

#endif
