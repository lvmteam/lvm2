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

struct vgname_entry {
	struct list pvdevs;
	char *vgname;
};

struct pvdev_list {
	struct list list;
	struct device *dev;
};

int vgcache_init();
void vgcache_destroy();

/* Return list of PVs in named VG */
struct list *vgcache_find(const char *vg_name);

/* Add/delete a device */
int vgcache_add(const char *vg_name, struct device *dev);
void vgcache_del(const char *vg_name);

#endif
