/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#ifndef _LVM_CACHE_H
#define _LVM_CACHE_H

#include "dev-cache.h"
#include "list.h"
#include "uuid.h"
#include "label.h"
#include "metadata.h"

#include <sys/types.h>

#define ORPHAN ""

#define CACHE_INVALID 0x00000001

/* LVM specific per-volume info */
/* Eventual replacement for struct physical_volume perhaps? */

struct cache_vginfo {
	struct list list;	/* Join these vginfos together */
	struct list infos;	/* List head for cache_infos */
	char *vgname;		/* "" == orphan */
	char vgid[ID_LEN + 1];
	struct format_type *fmt;
};

struct cache_info {
	struct list list;	/* Join VG members together */
	struct list mdas;	/* list head for metadata areas */
	struct list das;	/* list head for data areas */
	struct cache_vginfo *vginfo;	/* NULL == unknown */
	struct label *label;
	struct format_type *fmt;
	struct device *dev;
	uint64_t device_size;	/* Bytes */
	uint32_t status;
};

int cache_init();
void cache_destroy();

/* Set full_scan to 1 to reread every filtered device label */
int cache_label_scan(struct cmd_context *cmd, int full_scan);

/* Add/delete a device */
struct cache_info *cache_add(struct labeller *labeller, const char *pvid,
			     struct device *dev,
			     const char *vgname, const char *vgid);
void cache_del(struct cache_info *info);

/* Update things */
int cache_update_vgname(struct cache_info *info, const char *vgname);
int cache_update_vg(struct volume_group *vg);

/* Queries */
struct format_type *fmt_from_vgname(const char *vgname);
struct cache_vginfo *vginfo_from_vgname(const char *vgname);
struct cache_vginfo *vginfo_from_vgid(const char *vgid);
struct cache_info *info_from_pvid(const char *pvid);
struct device *device_from_pvid(struct cmd_context *cmd, struct id *pvid);

/* Returns list of struct str_lists containing pool-allocated copy of vgnames */
/* Set full_scan to 1 to reread every filtered device label */
struct list *cache_get_vgnames(struct cmd_context *cmd, int full_scan);

#endif
