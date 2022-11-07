/*
 * Copyright (C) 2013 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/device/device.h"

int device_id_list_remove(struct dm_list *list, struct device *dev)
{
	struct device_id_list *dil;

	dm_list_iterate_items(dil, list) {
		if (dil->dev == dev) {
			dm_list_del(&dil->list);
			return 1;
		}
	}
	return 0;
}

struct device_id_list *device_id_list_find_dev(struct dm_list *list, struct device *dev)
{
	struct device_id_list *dil;

	dm_list_iterate_items(dil, list) {
		if (dil->dev == dev)
			return dil;
	}
	return NULL;
}

int device_list_remove(struct dm_list *list, struct device *dev)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, list) {
		if (devl->dev == dev) {
			dm_list_del(&devl->list);
			return 1;
		}
	}
	return 0;
}

struct device_list *device_list_find_dev(struct dm_list *list, struct device *dev)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, list) {
		if (devl->dev == dev)
			return devl;
	}
	return NULL;
}

