/*
 * tools/lib/dev-manager.h
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 */

#ifndef _LVM_DEV_MANAGER_H
#define _LVM_DEV_MANAGER_H

#include <sys/types.h>
#include "config/config.h"

struct device {
	char *name;
	dev_t dev;
};

struct dev_mgr;
typedef struct _dummy_counter *dev_counter_t;

struct dev_mgr *init_dev_manager(struct config_node *cfg_node);
void fin_dev_manager(struct dev_mgr *dm);

struct device *dev_by_name(struct dev_mgr *dm, const char *name);
struct device *dev_by_dev(struct dev_mgr *dm, dev_t d);

/* either of these trigger a full scan, the first time they're run */
dev_counter_t init_dev_scan(struct dev_mgr *dm);
struct device *next_device(dev_counter_t *counter);
void fin_dev_scan(dev_counter_t counter);

#endif
