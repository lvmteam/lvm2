/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2010 Red Hat, Inc. All rights reserved.
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

#ifndef _SEGTYPES_H
#define _SEGTYPES_H

#include "metadata-exported.h"

struct segtype_handler;
struct cmd_context;
struct dm_config_tree;
struct lv_segment;
struct lv_activate_opts;
struct formatter;
struct dm_config_node;
struct dev_manager;

/* Feature flags */
#define SEG_CAN_SPLIT		0x00000001U
#define SEG_AREAS_STRIPED	0x00000002U
#define SEG_AREAS_MIRRORED	0x00000004U
#define SEG_SNAPSHOT		0x00000008U
#define SEG_FORMAT1_SUPPORT	0x00000010U
#define SEG_VIRTUAL		0x00000020U
#define SEG_CANNOT_BE_ZEROED	0x00000040U
#define SEG_MONITORED		0x00000080U
#define SEG_REPLICATOR		0x00000100U
#define SEG_REPLICATOR_DEV	0x00000200U
#define SEG_RAID		0x00000400U
#define SEG_THIN_POOL		0x00000800U
#define SEG_THIN_VOLUME		0x00001000U
#define SEG_CACHE		0x00002000U
#define SEG_CACHE_POOL		0x00004000U
#define SEG_MIRROR		0x00008000U
#define SEG_ONLY_EXCLUSIVE	0x00010000U /* In cluster only exlusive activation */
#define SEG_CAN_ERROR_WHEN_FULL	0x00020000U
#define SEG_UNKNOWN		0x80000000U

#define segtype_is_cache(segtype)	((segtype)->flags & SEG_CACHE ? 1 : 0)
#define segtype_is_cache_pool(segtype)	((segtype)->flags & SEG_CACHE_POOL ? 1 : 0)
#define segtype_is_mirrored(segtype)	((segtype)->flags & SEG_AREAS_MIRRORED ? 1 : 0)
#define segtype_is_mirror(segtype)	((segtype)->flags & SEG_MIRROR ? 1 : 0)
#define segtype_is_pool(segtype)	((segtype)->flags & (SEG_CACHE_POOL | SEG_THIN_POOL)  ? 1 : 0)
#define segtype_is_raid(segtype)	((segtype)->flags & SEG_RAID ? 1 : 0)
#define segtype_is_striped(segtype)	((segtype)->flags & SEG_AREAS_STRIPED ? 1 : 0)
#define segtype_is_thin(segtype)	((segtype)->flags & (SEG_THIN_POOL|SEG_THIN_VOLUME) ? 1 : 0)
#define segtype_is_thin_pool(segtype)	((segtype)->flags & SEG_THIN_POOL ? 1 : 0)
#define segtype_is_thin_volume(segtype)	((segtype)->flags & SEG_THIN_VOLUME ? 1 : 0)
#define segtype_is_virtual(segtype)	((segtype)->flags & SEG_VIRTUAL ? 1 : 0)
#define segtype_is_unknown(segtype)	((segtype)->flags & SEG_UNKNOWN ? 1 : 0)

#define seg_is_cache(seg)	segtype_is_cache((seg)->segtype)
#define seg_is_cache_pool(seg)	segtype_is_cache_pool((seg)->segtype)
#define seg_is_linear(seg)	(seg_is_striped(seg) && ((seg)->area_count == 1))
#define seg_is_mirror(seg)	segtype_is_mirror((seg)->segtype)
#define seg_is_mirrored(seg)	segtype_is_mirrored((seg)->segtype)
#define seg_is_pool(seg)	segtype_is_pool((seg)->segtype)
#define seg_is_raid(seg)	segtype_is_raid((seg)->segtype)
#define seg_is_replicator(seg)	((seg)->segtype->flags & SEG_REPLICATOR ? 1 : 0)
#define seg_is_replicator_dev(seg) ((seg)->segtype->flags & SEG_REPLICATOR_DEV ? 1 : 0)
#define seg_is_snapshot(seg)	((seg)->segtype->flags & SEG_SNAPSHOT ? 1 : 0)
#define seg_is_striped(seg)	segtype_is_striped((seg)->segtype)
#define seg_is_thin(seg)	segtype_is_thin((seg)->segtype)
#define seg_is_thin_pool(seg)	segtype_is_thin_pool((seg)->segtype)
#define seg_is_thin_volume(seg)	segtype_is_thin_volume((seg)->segtype)
#define seg_is_virtual(seg)	segtype_is_virtual((seg)->segtype)
#define seg_unknown(seg)	segtype_is_unknown((seg)->segtype)
#define seg_can_split(seg)	((seg)->segtype->flags & SEG_CAN_SPLIT ? 1 : 0)
#define seg_cannot_be_zeroed(seg) ((seg)->segtype->flags & SEG_CANNOT_BE_ZEROED ? 1 : 0)
#define seg_monitored(seg)	((seg)->segtype->flags & SEG_MONITORED ? 1 : 0)
#define seg_only_exclusive(seg)	((seg)->segtype->flags & SEG_ONLY_EXCLUSIVE ? 1 : 0)
#define seg_can_error_when_full(seg) ((seg)->segtype->flags & SEG_CAN_ERROR_WHEN_FULL ? 1 : 0)

struct segment_type {
	struct dm_list list;		/* Internal */

	uint32_t flags;
	uint32_t parity_devs;           /* Parity drives required by segtype */

	struct segtype_handler *ops;
	const char *name;

	void *library;			/* lvm_register_segtype() sets this. */
	void *private;			/* For the segtype handler to use. */
};

struct segtype_handler {
	const char *(*name) (const struct lv_segment * seg);
	const char *(*target_name) (const struct lv_segment *seg,
				    const struct lv_activate_opts *laopts);
	void (*display) (const struct lv_segment * seg);
	int (*text_export) (const struct lv_segment * seg,
			    struct formatter * f);
	int (*text_import_area_count) (const struct dm_config_node * sn,
				       uint32_t *area_count);
	int (*text_import) (struct lv_segment * seg,
			    const struct dm_config_node * sn,
			    struct dm_hash_table * pv_hash);
	int (*merge_segments) (struct lv_segment * seg1,
			       struct lv_segment * seg2);
	int (*add_target_line) (struct dev_manager *dm, struct dm_pool *mem,
				struct cmd_context *cmd, void **target_state,
				struct lv_segment *seg,
				const struct lv_activate_opts *laopts,
				struct dm_tree_node *node, uint64_t len,
				uint32_t *pvmove_mirror_count);
	int (*target_status_compatible) (const char *type);
	int (*check_transient_status) (struct lv_segment *seg, char *params);
	int (*target_percent) (void **target_state,
			       dm_percent_t *percent,
			       struct dm_pool * mem,
			       struct cmd_context *cmd,
			       struct lv_segment *seg, char *params,
			       uint64_t *total_numerator,
			       uint64_t *total_denominator);
	int (*target_present) (struct cmd_context *cmd,
			       const struct lv_segment *seg,
			       unsigned *attributes);
	int (*modules_needed) (struct dm_pool *mem,
			       const struct lv_segment *seg,
			       struct dm_list *modules);
	void (*destroy) (struct segment_type * segtype);
	int (*target_monitored) (struct lv_segment *seg, int *pending);
	int (*target_monitor_events) (struct lv_segment *seg, int events);
	int (*target_unmonitor_events) (struct lv_segment *seg, int events);
};

struct segment_type *get_segtype_from_string(struct cmd_context *cmd,
					     const char *str);

struct segtype_library;
int lvm_register_segtype(struct segtype_library *seglib,
			 struct segment_type *segtype);

struct segment_type *init_striped_segtype(struct cmd_context *cmd);
struct segment_type *init_zero_segtype(struct cmd_context *cmd);
struct segment_type *init_error_segtype(struct cmd_context *cmd);
struct segment_type *init_free_segtype(struct cmd_context *cmd);
struct segment_type *init_unknown_segtype(struct cmd_context *cmd,
					  const char *name);

#define RAID_FEATURE_RAID10			(1U << 0) /* version 1.3 */

#ifdef RAID_INTERNAL
int init_raid_segtypes(struct cmd_context *cmd, struct segtype_library *seglib);
#endif

#define SEG_TYPE_NAME_RAID1	"raid1"
#define SEG_TYPE_NAME_RAID10	"raid10"
#define SEG_TYPE_NAME_RAID4	"raid4"
#define SEG_TYPE_NAME_RAID5	"raid5"
#define SEG_TYPE_NAME_RAID5_LA	"raid5_la"
#define SEG_TYPE_NAME_RAID5_LS	"raid5_ls"
#define SEG_TYPE_NAME_RAID5_RA	"raid5_ra"
#define SEG_TYPE_NAME_RAID5_RS	"raid5_rs"
#define SEG_TYPE_NAME_RAID6	"raid6"
#define SEG_TYPE_NAME_RAID6_NC	"raid6_nc"
#define SEG_TYPE_NAME_RAID6_NR	"raid6_nr"
#define SEG_TYPE_NAME_RAID6_ZR	"raid6_zr"

#ifdef REPLICATOR_INTERNAL
int init_replicator_segtype(struct cmd_context *cmd, struct segtype_library *seglib);
#endif

#define THIN_FEATURE_DISCARDS			(1U << 0)
#define THIN_FEATURE_EXTERNAL_ORIGIN		(1U << 1)
#define THIN_FEATURE_HELD_ROOT			(1U << 2)
#define THIN_FEATURE_BLOCK_SIZE			(1U << 3)
#define THIN_FEATURE_DISCARDS_NON_POWER_2	(1U << 4)
#define THIN_FEATURE_METADATA_RESIZE		(1U << 5)
#define THIN_FEATURE_EXTERNAL_ORIGIN_EXTEND	(1U << 6)
#define THIN_FEATURE_ERROR_IF_NO_SPACE		(1U << 7)

#ifdef THIN_INTERNAL
int init_thin_segtypes(struct cmd_context *cmd, struct segtype_library *seglib);
#endif

#ifdef CACHE_INTERNAL
int init_cache_segtypes(struct cmd_context *cmd, struct segtype_library *seglib);
#endif

#define SNAPSHOT_FEATURE_FIXED_LEAK		(1U << 0) /* version 1.12 */

#ifdef SNAPSHOT_INTERNAL
struct segment_type *init_snapshot_segtype(struct cmd_context *cmd);
#endif

#define MIRROR_LOG_CLUSTERED			(1U << 0)

#ifdef MIRRORED_INTERNAL
struct segment_type *init_mirrored_segtype(struct cmd_context *cmd);
#endif

#ifdef CRYPT_INTERNAL
struct segment_type *init_crypt_segtype(struct cmd_context *cmd);
#endif

#endif
