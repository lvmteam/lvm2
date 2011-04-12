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
#ifndef _LVM_LV_H
#define _LVM_LV_H

union lvid;
struct volume_group;
struct dm_list;
struct lv_segment;
struct replicator_device;

struct logical_volume {
	union lvid lvid;
	const char *name;

	struct volume_group *vg;

	uint64_t status;
	alloc_policy_t alloc;
	uint32_t read_ahead;
	int32_t major;
	int32_t minor;

	uint64_t size;		/* Sectors */
	uint32_t le_count;

	uint32_t origin_count;
	struct dm_list snapshot_segs;
	struct lv_segment *snapshot;

	struct replicator_device *rdevice;/* For replicator-devs, rimages, slogs - reference to rdevice */
	struct dm_list rsites;	/* For replicators - all sites */

	struct dm_list segments;
	struct dm_list tags;
	struct dm_list segs_using_this_lv;
};

uint64_t lv_size(const struct logical_volume *lv);
char *lv_attr_dup(struct dm_pool *mem, const struct logical_volume *lv);
char *lv_uuid_dup(const struct logical_volume *lv);
char *lv_tags_dup(const struct logical_volume *lv);
char *lv_path_dup(struct dm_pool *mem, const struct logical_volume *lv);
uint64_t lv_origin_size(const struct logical_volume *lv);
char *lv_move_pv_dup(struct dm_pool *mem, const struct logical_volume *lv);
char *lv_convert_lv_dup(struct dm_pool *mem, const struct logical_volume *lv);
int lv_kernel_major(const struct logical_volume *lv);
int lv_kernel_minor(const struct logical_volume *lv);
char *lv_mirror_log_dup(struct dm_pool *mem, const struct logical_volume *lv);
char *lv_modules_dup(struct dm_pool *mem, const struct logical_volume *lv);
char *lv_name_dup(struct dm_pool *mem, const struct logical_volume *lv);
char *lv_origin_dup(struct dm_pool *mem, const struct logical_volume *lv);
uint32_t lv_kernel_read_ahead(const struct logical_volume *lv);
uint64_t lvseg_start(const struct lv_segment *seg);
uint64_t lvseg_size(const struct lv_segment *seg);
uint64_t lvseg_chunksize(const struct lv_segment *seg);
char *lvseg_segtype_dup(struct dm_pool *mem, const struct lv_segment *seg);
char *lvseg_tags_dup(const struct lv_segment *seg);
char *lvseg_devices(struct dm_pool *mem, const struct lv_segment *seg);
char *lvseg_seg_pe_ranges(struct dm_pool *mem, const struct lv_segment *seg);

#endif /* _LVM_LV_H */
