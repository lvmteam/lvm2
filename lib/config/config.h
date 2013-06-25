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
#include "defaults.h"

/* 16 bits: 3 bits for major, 4 bits for minor, 9 bits for patchlevel */
/* Max LVM version supported: 7.15.511. Just extend bits if ever needed. */
#define vsn(major, minor, patchlevel) (major << 13 | minor << 9 | patchlevel)

struct device;
struct cmd_context;

#define CFG_PATH_MAX_LEN 64

/*
 * Structures used for definition of a configuration tree.
 */

/* configuration definition item type (for item's accepted types) */
typedef enum {
	CFG_TYPE_SECTION =	1 << 0,	/* section */
	CFG_TYPE_ARRAY =	1 << 1,	/* setting */
	CFG_TYPE_BOOL =		1 << 2,	/* setting */
	CFG_TYPE_INT =		1 << 3,	/* setting */
	CFG_TYPE_FLOAT =	1 << 4,	/* setting */
	CFG_TYPE_STRING =	1 << 5,	/* setting */
} cfg_def_type_t;

/* configuration definition item value (for item's default value) */
typedef union {
	const int v_CFG_TYPE_BOOL, v_CFG_TYPE_INT;
	const float v_CFG_TYPE_FLOAT;
	const char *v_CFG_TYPE_STRING, *v_CFG_TYPE_ARRAY;
} cfg_def_value_t;

/* configuration definition item flags: */

/* whether the configuration item name is variable */
#define CFG_NAME_VARIABLE	0x01
/* whether empty value is allowed */
#define CFG_ALLOW_EMPTY		0x02
/* whether the configuration item is for advanced use only */
#define CFG_ADVANCED		0x04
/* whether the configuraton item is not officially supported */
#define CFG_UNSUPPORTED		0x08
/* helper flag to mark the item as used in a config tree instance */
#define CFG_USED		0x10
/* helper flag to mark the item as valid in a config tree instance */
#define CFG_VALID		0x20

/* configuration definition item structure */
typedef struct cfg_def_item {
	int id;				/* ID of this item */
	int parent;			/* ID of parent item */
	const char *name;		/* name of the item in configuration tree */
	cfg_def_type_t type;		/* configuration item type */
	cfg_def_value_t default_value;	/* default value (only for settings) */
	uint16_t flags;			/* configuration item definition flags */
	uint16_t since_version;		/* version this item appeared in */
	const char *comment;		/* brief comment */
} cfg_def_item_t;

/* configuration definition tree types */
typedef enum {
	CFG_DEF_TREE_CURRENT,		/* tree of nodes with values currently set in the config */
	CFG_DEF_TREE_MISSING,		/* tree of nodes missing in current config using default values */
	CFG_DEF_TREE_COMPLETE,		/* CURRENT + MISSING, the tree actually used within execution, not implemented yet */
	CFG_DEF_TREE_DEFAULT,		/* tree of all possible config nodes with default values */
	CFG_DEF_TREE_NEW		/* tree of all new nodes that appeared in given version */
} cfg_def_tree_t;

/* configuration definition tree specification */
struct config_def_tree_spec {
	cfg_def_tree_t type;		/* tree type */
	uint16_t version;		/* tree at this LVM2 version */
	int ignoreadvanced;		/* do not include advanced configs */
	int ignoreunsupported;		/* do not include unsupported configs */
};

/*
 * Register ID for each possible item in the configuration tree.
 */
enum {
#define cfg_section(id, name, parent, flags, since_version, comment) id,
#define cfg(id, name, parent, flags, type, default_value, since_version, comment) id,
#define cfg_array(id, name, parent, flags, types, default_value, since_version, comment) id,
#include "config_settings.h"
#undef cfg_section
#undef cfg
#undef cfg_array
};

int config_def_get_path(char *buf, size_t buf_size, int id);
int config_def_check(struct cmd_context *cmd, int force, int skip, int suppress_messages);

int override_config_tree_from_string(struct cmd_context *cmd,
				     const char *config_settings);
struct dm_config_tree *remove_overridden_config_tree(struct cmd_context *cmd);

typedef uint32_t (*checksum_fn_t) (uint32_t initial, const uint8_t *buf, uint32_t size);

struct dm_config_tree *config_file_open(const char *filename, int keep_open);
int config_file_read_fd(struct dm_config_tree *cft, struct device *dev,
			off_t offset, size_t size, off_t offset2, size_t size2,
			checksum_fn_t checksum_fn, uint32_t checksum);
int config_file_read(struct dm_config_tree *cft);
struct dm_config_tree *config_file_open_and_read(const char *config_file);
int config_write(struct dm_config_tree *cft,
		 int withcomment, int withversion,
		 const char *file, int argc, char **argv);
struct dm_config_tree *config_def_create_tree(struct config_def_tree_spec *spec);
void config_file_destroy(struct dm_config_tree *cft);

time_t config_file_timestamp(struct dm_config_tree *cft);
int config_file_changed(struct dm_config_tree *cft);
int config_file_check(struct dm_config_tree *cft, const char **filename, struct stat *info);


int merge_config_tree(struct cmd_context *cmd, struct dm_config_tree *cft,
		      struct dm_config_tree *newdata);

/*
 * These versions check an override tree, if present, first.
 */
const struct dm_config_node *find_config_tree_node(struct cmd_context *cmd, int id);
const char *find_config_tree_str(struct cmd_context *cmd, int id);
const char *find_config_tree_str_allow_empty(struct cmd_context *cmd, int id);
int find_config_tree_int(struct cmd_context *cmd, int id);
int64_t find_config_tree_int64(struct cmd_context *cmd, int id);
float find_config_tree_float(struct cmd_context *cmd, int id);
int find_config_tree_bool(struct cmd_context *cmd, int id);

#endif
