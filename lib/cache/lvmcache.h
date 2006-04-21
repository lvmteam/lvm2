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
 *
 */

#ifndef _LVM_CACHE_H
#define _LVM_CACHE_H

#include "dev-cache.h"
#include "uuid.h"
#include "label.h"

#define ORPHAN ""

#define CACHE_INVALID	0x00000001
#define CACHE_LOCKED	0x00000002

/* LVM specific per-volume info */
/* Eventual replacement for struct physical_volume perhaps? */

struct cmd_context;
struct format_type;
struct volume_group;

/* One per VG */
struct lvmcache_vginfo {
	struct list list;	/* Join these vginfos together */
	struct list infos;	/* List head for lvmcache_infos */
	const struct format_type *fmt;
	char *vgname;		/* "" == orphan */
	uint32_t status;
	char vgid[ID_LEN + 1];
	char _padding[7];
	struct lvmcache_vginfo *next; /* Another VG with same name? */
	char *creation_host;
};

/* One per device */
struct lvmcache_info {
	struct list list;	/* Join VG members together */
	struct list mdas;	/* list head for metadata areas */
	struct list das;	/* list head for data areas */
	struct lvmcache_vginfo *vginfo;	/* NULL == unknown */
	struct label *label;
	const struct format_type *fmt;
	struct device *dev;
	uint64_t device_size;	/* Bytes */
	uint32_t status;
};

int lvmcache_init(void);
void lvmcache_destroy(void);

/* Set full_scan to 1 to reread every filtered device label or
 * 2 to rescan /dev for new devices */
int lvmcache_label_scan(struct cmd_context *cmd, int full_scan);

/* Add/delete a device */
struct lvmcache_info *lvmcache_add(struct labeller *labeller, const char *pvid,
				   struct device *dev,
				   const char *vgname, const char *vgid,
				   uint32_t vgstatus);
void lvmcache_del(struct lvmcache_info *info);

/* Update things */
int lvmcache_update_vgname_and_id(struct lvmcache_info *info,
				  const char *vgname, const char *vgid,
				  uint32_t vgstatus, const char *hostname);
int lvmcache_update_vg(struct volume_group *vg);

void lvmcache_lock_vgname(const char *vgname, int read_only);
void lvmcache_unlock_vgname(const char *vgname);

/* Queries */
const struct format_type *fmt_from_vgname(const char *vgname, const char *vgid);
struct lvmcache_vginfo *vginfo_from_vgname(const char *vgname,
					   const char *vgid);
struct lvmcache_vginfo *vginfo_from_vgid(const char *vgid);
struct lvmcache_info *info_from_pvid(const char *pvid);
const char *vgname_from_vgid(struct dm_pool *mem, const char *vgid);
struct device *device_from_pvid(struct cmd_context *cmd, struct id *pvid);
int vgs_locked(void);
int vgname_is_locked(const char *vgname);

/* Returns list of struct str_lists containing pool-allocated copy of vgnames */
/* Set full_scan to 1 to reread every filtered device label */
struct list *lvmcache_get_vgnames(struct cmd_context *cmd, int full_scan);

/* Returns list of struct str_lists containing pool-allocated copy of vgids */
/* Set full_scan to 1 to reread every filtered device label */
struct list *lvmcache_get_vgids(struct cmd_context *cmd, int full_scan);

/* Returns list of struct str_lists containing pool-allocated copy of pvids */
struct list *lvmcache_get_pvids(struct cmd_context *cmd, const char *vgname,
				const char *vgid);

#endif
