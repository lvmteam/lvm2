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

#ifndef _SEGTYPES_H
#define _SEGTYPES_H

struct segtype_handler;
struct cmd_context;
struct config_tree;
struct lv_segment;
struct formatter;
struct config_node;
struct hash_table;
struct dev_manager;

/* Feature flags */
#define SEG_CAN_SPLIT		0x00000001
#define SEG_AREAS_STRIPED	0x00000002
#define SEG_AREAS_MIRRORED	0x00000004
#define SEG_SNAPSHOT		0x00000008
#define SEG_FORMAT1_SUPPORT	0x00000010
#define SEG_VIRTUAL		0x00000020

struct segment_type {
	struct list list;
	struct cmd_context *cmd;
	uint32_t flags;
	struct segtype_handler *ops;
	const char *name;
	void *library;
	void *private;
};

struct segtype_handler {
	const char *(*name) (const struct lv_segment * seg);
	void (*display) (const struct lv_segment * seg);
	int (*text_export) (const struct lv_segment * seg,
			    struct formatter * f);
	int (*text_import_area_count) (struct config_node * sn,
				       uint32_t *area_count);
	int (*text_import) (struct lv_segment * seg,
			    const struct config_node * sn,
			    struct hash_table * pv_hash);
	int (*merge_segments) (struct lv_segment * seg1,
			       struct lv_segment * seg2);
	int (*compose_target_line) (struct dev_manager * dm, struct pool * mem,
				    struct config_tree * cft,
				    void **target_state,
				    struct lv_segment * seg, char *params,
				    size_t paramsize, const char **target,
				    int *pos, uint32_t *pvmove_mirror_count);
	int (*target_percent) (void **target_state, struct pool * mem,
			       struct config_tree * cft,
			       struct lv_segment * seg, char *params,
			       uint64_t *total_numerator,
			       uint64_t *total_denominator, float *percent);
	int (*target_present) (void);
	void (*destroy) (const struct segment_type * segtype);
};

struct segment_type *get_segtype_from_string(struct cmd_context *cmd,
					     const char *str);

struct segment_type *init_striped_segtype(struct cmd_context *cmd);
struct segment_type *init_zero_segtype(struct cmd_context *cmd);
struct segment_type *init_error_segtype(struct cmd_context *cmd);

#ifdef SNAPSHOT_INTERNAL
struct segment_type *init_snapshot_segtype(struct cmd_context *cmd);
#endif

#ifdef MIRRORED_INTERNAL
struct segment_type *init_mirrored_segtype(struct cmd_context *cmd);
#endif

#endif
