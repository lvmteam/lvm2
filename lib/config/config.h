/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_CONFIG_H
#define _LVM_CONFIG_H

#include "device.h"

enum {
	CFG_STRING,
	CFG_FLOAT,
	CFG_INT,
	CFG_EMPTY_ARRAY
};

struct config_value {
	int type;
	union {
		int i;
		float r;
		char *str;
	} v;
	struct config_value *next;	/* for arrays */
};

struct config_node {
	char *key;
	struct config_node *sib, *child;
	struct config_value *v;
};

struct config_tree {
	struct config_node *root;
};

struct config_tree *create_config_tree(void);
void destroy_config_tree(struct config_tree *cf);

typedef uint32_t (*checksum_fn_t) (uint32_t initial, void *buf, uint32_t size);

int read_config_fd(struct config_tree *cf, struct device *dev,
		   off_t offset, size_t size, off_t offset2, size_t size2,
		   checksum_fn_t checksum_fn, uint32_t checksum);

int read_config_file(struct config_tree *cf, const char *file);
int write_config_file(struct config_tree *cf, const char *file);
int reload_config_file(struct config_tree **cf);
time_t config_file_timestamp(struct config_tree *cf);

struct config_node *find_config_node(struct config_node *cn,
				     const char *path);

const char *find_config_str(struct config_node *cn, const char *path,
			    const char *fail);

int find_config_int(struct config_node *cn, const char *path, int fail);

float find_config_float(struct config_node *cn, const char *path, float fail);

/*
 * Understands (0, ~0), (y, n), (yes, no), (on,
 * off), (true, false).
 */
int find_config_bool(struct config_node *cn, const char *path, int fail);

int get_config_uint32(struct config_node *cn, const char *path,
		      uint32_t *result);

int get_config_uint64(struct config_node *cn, const char *path,
		      uint64_t *result);

int get_config_str(struct config_node *cn, const char *path, char **result);

#endif
