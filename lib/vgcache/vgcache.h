/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#ifndef _LVM_VGCACHE_H
#define _LVM_VGCACHE_H

#include <sys/types.h>
#include <asm/page.h>
#include "dev-cache.h"
#include "list.h"
#include "uuid.h"
#include "toolcontext.h"

struct vgname_entry {
	struct list pvdevs;
	char *vgname;
	char vgid[ID_LEN + 1];
	struct format_type *fmt;
};

struct pvdev_list {
	struct list list;
	struct device *dev;
};

int vgcache_init();
void vgcache_destroy();

/* Return list of PVs in named VG */
struct list *vgcache_find(const char *vg_name);
struct format_type *vgcache_find_format(const char *vg_name);
struct list *vgcache_find_by_vgid(const char *vgid);

/* FIXME Temporary function */
char *vgname_from_vgid(struct cmd_context *cmd, struct id *vgid);

/* Add/delete a device */
int vgcache_add(const char *vg_name, const char *vgid, struct device *dev,
		struct format_type *fmt);
void vgcache_del(const char *vg_name);

#endif
