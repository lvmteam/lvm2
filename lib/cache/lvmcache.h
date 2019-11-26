/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2008 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_CACHE_H
#define _LVM_CACHE_H

#include "lib/device/dev-cache.h"
#include "lib/device/dev-type.h"
#include "lib/uuid/uuid.h"
#include "lib/label/label.h"
#include "lib/locking/locking.h"

#define ORPHAN_PREFIX VG_ORPHANS
#define ORPHAN_VG_NAME(fmt) ORPHAN_PREFIX "_" fmt

/* LVM specific per-volume info */
/* Eventual replacement for struct physical_volume perhaps? */

struct cmd_context;
struct format_type;
struct volume_group;
struct physical_volume;
struct dm_config_tree;
struct format_instance;
struct metadata_area;
struct disk_locn;

struct lvmcache_vginfo;

/*
 * vgsummary represents a summary of the VG that is read
 * without a lock during label scan.  It's used to populate
 * basic lvmcache vginfo/info during label scan prior to
 * vg_read().
 */
struct lvmcache_vgsummary {
	const char *vgname;
	struct id vgid;
	uint64_t vgstatus;
	char *creation_host;
	const char *system_id;
	const char *lock_type;
	uint32_t seqno;
	uint32_t mda_checksum;
	size_t mda_size;
	int mda_num; /* 1 = summary from mda1, 2 = summary from mda2 */
	unsigned mda_ignored:1;
	unsigned zero_offset:1;
	unsigned mismatch:1; /* lvmcache sets if this summary differs from previous values */
	struct dm_list pvsummaries;
};

int lvmcache_init(struct cmd_context *cmd);

void lvmcache_destroy(struct cmd_context *cmd, int retain_orphans, int reset);

int lvmcache_label_scan(struct cmd_context *cmd);
int lvmcache_label_rescan_vg(struct cmd_context *cmd, const char *vgname, const char *vgid);
int lvmcache_label_rescan_vg_rw(struct cmd_context *cmd, const char *vgname, const char *vgid);

/* Add/delete a device */
struct lvmcache_info *lvmcache_add(struct labeller *labeller, const char *pvid,
                                   struct device *dev, uint64_t label_sector,
                                   const char *vgname, const char *vgid,
                                   uint32_t vgstatus, int *is_duplicate);
int lvmcache_add_orphan_vginfo(const char *vgname, struct format_type *fmt);
void lvmcache_del(struct lvmcache_info *info);
void lvmcache_del_dev(struct device *dev);

/* Update things */
int lvmcache_update_vgname_and_id(struct lvmcache_info *info,
				  struct lvmcache_vgsummary *vgsummary);
int lvmcache_update_vg_from_read(struct volume_group *vg, unsigned precommitted);
int lvmcache_update_vg_from_write(struct volume_group *vg);

void lvmcache_lock_vgname(const char *vgname, int read_only);
void lvmcache_unlock_vgname(const char *vgname);

/* Queries */
int lvmcache_lookup_mda(struct lvmcache_vgsummary *vgsummary);

struct lvmcache_vginfo *lvmcache_vginfo_from_vgname(const char *vgname,
					   const char *vgid);
struct lvmcache_vginfo *lvmcache_vginfo_from_vgid(const char *vgid);
struct lvmcache_info *lvmcache_info_from_pvid(const char *pvid, struct device *dev, int valid_only);
const char *lvmcache_vgname_from_vgid(struct dm_pool *mem, const char *vgid);
const char *lvmcache_vgid_from_vgname(struct cmd_context *cmd, const char *vgname);
struct device *lvmcache_device_from_pvid(struct cmd_context *cmd, const struct id *pvid, uint64_t *label_sector);
const char *lvmcache_vgname_from_info(struct lvmcache_info *info);
const struct format_type *lvmcache_fmt_from_info(struct lvmcache_info *info);

int lvmcache_get_vgnameids(struct cmd_context *cmd,
                           struct dm_list *vgnameids,
                           const char *only_this_vgname,
                           int include_internal);

void lvmcache_drop_metadata(const char *vgname, int drop_precommitted);
void lvmcache_commit_metadata(const char *vgname);

int lvmcache_fid_add_mdas(struct lvmcache_info *info, struct format_instance *fid,
			  const char *id, int id_len);
int lvmcache_fid_add_mdas_pv(struct lvmcache_info *info, struct format_instance *fid);
int lvmcache_fid_add_mdas_vg(struct lvmcache_vginfo *vginfo, struct format_instance *fid);
int lvmcache_populate_pv_fields(struct lvmcache_info *info,
				struct volume_group *vg,
				struct physical_volume *pv);
int lvmcache_check_format(struct lvmcache_info *info, const struct format_type *fmt);
void lvmcache_del_mdas(struct lvmcache_info *info);
void lvmcache_del_das(struct lvmcache_info *info);
void lvmcache_del_bas(struct lvmcache_info *info);
int lvmcache_add_mda(struct lvmcache_info *info, struct device *dev,
		     uint64_t start, uint64_t size, unsigned ignored,
		     struct metadata_area **mda_new);
int lvmcache_add_da(struct lvmcache_info *info, uint64_t start, uint64_t size);
int lvmcache_add_ba(struct lvmcache_info *info, uint64_t start, uint64_t size);

void lvmcache_set_ext_version(struct lvmcache_info *info, uint32_t version);
uint32_t lvmcache_ext_version(struct lvmcache_info *info);
void lvmcache_set_ext_flags(struct lvmcache_info *info, uint32_t flags);
uint32_t lvmcache_ext_flags(struct lvmcache_info *info);

const struct format_type *lvmcache_fmt(struct lvmcache_info *info);
struct label *lvmcache_get_label(struct lvmcache_info *info);
struct label *lvmcache_get_dev_label(struct device *dev);
int lvmcache_has_dev_info(struct device *dev);

void lvmcache_update_pv(struct lvmcache_info *info, struct physical_volume *pv,
			const struct format_type *fmt);
int lvmcache_update_das(struct lvmcache_info *info, struct physical_volume *pv);
int lvmcache_update_bas(struct lvmcache_info *info, struct physical_volume *pv);
int lvmcache_foreach_mda(struct lvmcache_info *info,
			 int (*fun)(struct metadata_area *, void *),
			 void *baton);

int lvmcache_foreach_da(struct lvmcache_info *info,
			int (*fun)(struct disk_locn *, void *),
			void *baton);

int lvmcache_foreach_ba(struct lvmcache_info *info,
			int (*fun)(struct disk_locn *, void *),
			void *baton);

int lvmcache_foreach_pv(struct lvmcache_vginfo *vginfo,
			int (*fun)(struct lvmcache_info *, void *), void * baton);

uint64_t lvmcache_device_size(struct lvmcache_info *info);
void lvmcache_set_device_size(struct lvmcache_info *info, uint64_t size);
struct device *lvmcache_device(struct lvmcache_info *info);
unsigned lvmcache_mda_count(struct lvmcache_info *info);
uint64_t lvmcache_smallest_mda_size(struct lvmcache_info *info);

struct metadata_area *lvmcache_get_mda(struct cmd_context *cmd,
                                      const char *vgname,
                                      struct device *dev,
                                      int use_mda_num);

bool lvmcache_has_duplicate_devs(void);
void lvmcache_del_dev_from_duplicates(struct device *dev);
bool lvmcache_dev_is_unused_duplicate(struct device *dev);
int lvmcache_pvid_in_unused_duplicates(const char *pvid);
int lvmcache_get_unused_duplicates(struct cmd_context *cmd, struct dm_list *head);
int vg_has_duplicate_pvs(struct volume_group *vg);

int lvmcache_found_duplicate_vgnames(void);

int lvmcache_contains_lock_type_sanlock(struct cmd_context *cmd);

void lvmcache_get_max_name_lengths(struct cmd_context *cmd,
			unsigned *pv_max_name_len, unsigned *vg_max_name_len);

int lvmcache_vg_is_foreign(struct cmd_context *cmd, const char *vgname, const char *vgid);

bool lvmcache_scan_mismatch(struct cmd_context *cmd, const char *vgname, const char *vgid);

int lvmcache_vginfo_has_pvid(struct lvmcache_vginfo *vginfo, char *pvid);

uint64_t lvmcache_max_metadata_size(void);
void lvmcache_save_metadata_size(uint64_t val);

int dev_in_device_list(struct device *dev, struct dm_list *head);

bool lvmcache_has_bad_metadata(struct device *dev);

bool lvmcache_has_old_metadata(struct cmd_context *cmd, const char *vgname, const char *vgid, struct device *dev);

void lvmcache_get_outdated_devs(struct cmd_context *cmd,
                                const char *vgname, const char *vgid,
                                struct dm_list *devs);
void lvmcache_get_outdated_mdas(struct cmd_context *cmd,
                                const char *vgname, const char *vgid,
                                struct device *dev,
                                struct dm_list **mdas);

bool lvmcache_is_outdated_dev(struct cmd_context *cmd,
                              const char *vgname, const char *vgid,
                              struct device *dev);

void lvmcache_del_outdated_devs(struct cmd_context *cmd,
                                const char *vgname, const char *vgid);

void lvmcache_save_bad_mda(struct lvmcache_info *info, struct metadata_area *mda);

void lvmcache_get_bad_mdas(struct cmd_context *cmd,
                           const char *vgname, const char *vgid,
                           struct dm_list *bad_mda_list);

void lvmcache_get_mdas(struct cmd_context *cmd,
                       const char *vgname, const char *vgid,
                       struct dm_list *mda_list);

#endif
