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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_CACHE_H
#define _LVM_CACHE_H

#include "dev-cache.h"
#include "dev-type.h"
#include "uuid.h"
#include "label.h"
#include "locking.h"

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

int lvmcache_init(void);
void lvmcache_allow_reads_with_lvmetad(void);

void lvmcache_destroy(struct cmd_context *cmd, int retain_orphans);

/* Set full_scan to 1 to reread every filtered device label or
 * 2 to rescan /dev for new devices */
int lvmcache_label_scan(struct cmd_context *cmd, int full_scan);

/* Add/delete a device */
struct lvmcache_info *lvmcache_add(struct labeller *labeller, const char *pvid,
				   struct device *dev,
				   const char *vgname, const char *vgid,
				   uint32_t vgstatus);
int lvmcache_add_orphan_vginfo(const char *vgname, struct format_type *fmt);
void lvmcache_del(struct lvmcache_info *info);

/* Update things */
int lvmcache_update_vgname_and_id(struct lvmcache_info *info,
				  const char *vgname, const char *vgid,
				  uint32_t vgstatus, const char *hostname);
int lvmcache_update_vg(struct volume_group *vg, unsigned precommitted);

void lvmcache_lock_vgname(const char *vgname, int read_only);
void lvmcache_unlock_vgname(const char *vgname);
int lvmcache_verify_lock_order(const char *vgname);

/* Queries */
const struct format_type *lvmcache_fmt_from_vgname(struct cmd_context *cmd, const char *vgname, const char *vgid, unsigned revalidate_labels);

/* Decrement and test if there are still vg holders in vginfo. */
int lvmcache_vginfo_holders_dec_and_test_for_zero(struct lvmcache_vginfo *vginfo);

struct lvmcache_vginfo *lvmcache_vginfo_from_vgname(const char *vgname,
					   const char *vgid);
struct lvmcache_vginfo *lvmcache_vginfo_from_vgid(const char *vgid);
struct lvmcache_info *lvmcache_info_from_pvid(const char *pvid, int valid_only);
const char *lvmcache_vgname_from_vgid(struct dm_pool *mem, const char *vgid);
struct device *lvmcache_device_from_pvid(struct cmd_context *cmd, const struct id *pvid,
				unsigned *scan_done_once, uint64_t *label_sector);
const char *lvmcache_pvid_from_devname(struct cmd_context *cmd,
			      const char *dev_name);
char *lvmcache_vgname_from_pvid(struct cmd_context *cmd, const char *pvid);
const char *lvmcache_vgname_from_info(struct lvmcache_info *info);
int lvmcache_vgs_locked(void);
int lvmcache_vgname_is_locked(const char *vgname);

void lvmcache_seed_infos_from_lvmetad(struct cmd_context *cmd);

/* Returns list of struct str_lists containing pool-allocated copy of vgnames */
/* If include_internal is not set, return only proper vg names. */
struct dm_list *lvmcache_get_vgnames(struct cmd_context *cmd,
				     int include_internal);

/* Returns list of struct str_lists containing pool-allocated copy of vgids */
/* If include_internal is not set, return only proper vg ids. */
struct dm_list *lvmcache_get_vgids(struct cmd_context *cmd,
				   int include_internal);

/* Returns list of struct str_lists containing pool-allocated copy of pvids */
struct dm_list *lvmcache_get_pvids(struct cmd_context *cmd, const char *vgname,
				const char *vgid);

/* Returns cached volume group metadata. */
struct volume_group *lvmcache_get_vg(struct cmd_context *cmd, const char *vgname,
				     const char *vgid, unsigned precommitted);
void lvmcache_drop_metadata(const char *vgname, int drop_precommitted);
void lvmcache_commit_metadata(const char *vgname);

int lvmcache_pvid_is_locked(const char *pvid);
int lvmcache_fid_add_mdas(struct lvmcache_info *info, struct format_instance *fid,
			  const char *id, int id_len);
int lvmcache_fid_add_mdas_pv(struct lvmcache_info *info, struct format_instance *fid);
int lvmcache_fid_add_mdas_vg(struct lvmcache_vginfo *vginfo, struct format_instance *fid);
int lvmcache_populate_pv_fields(struct lvmcache_info *info,
				struct physical_volume *pv,
				int scan_label_only);
int lvmcache_check_format(struct lvmcache_info *info, const struct format_type *fmt);
void lvmcache_del_mdas(struct lvmcache_info *info);
void lvmcache_del_das(struct lvmcache_info *info);
void lvmcache_del_bas(struct lvmcache_info *info);
int lvmcache_add_mda(struct lvmcache_info *info, struct device *dev,
		     uint64_t start, uint64_t size, unsigned ignored);
int lvmcache_add_da(struct lvmcache_info *info, uint64_t start, uint64_t size);
int lvmcache_add_ba(struct lvmcache_info *info, uint64_t start, uint64_t size);

const struct format_type *lvmcache_fmt(struct lvmcache_info *info);
struct label *lvmcache_get_label(struct lvmcache_info *info);

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

int lvmcache_foreach_pv(struct lvmcache_vginfo *vg,
			int (*fun)(struct lvmcache_info *, void *), void * baton);

uint64_t lvmcache_device_size(struct lvmcache_info *info);
void lvmcache_set_device_size(struct lvmcache_info *info, uint64_t size);
struct device *lvmcache_device(struct lvmcache_info *info);
void lvmcache_make_valid(struct lvmcache_info *info);
int lvmcache_is_orphan(struct lvmcache_info *info);
int lvmcache_uncertain_ownership(struct lvmcache_info *info);
unsigned lvmcache_mda_count(struct lvmcache_info *info);
int lvmcache_vgid_is_cached(const char *vgid);
uint64_t lvmcache_smallest_mda_size(struct lvmcache_info *info);

#endif
