/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_LL_ACTIVATE_H
#define _LVM_LL_ACTIVATE_H

#include "metadata.h"

#include <libdevmapper.h>

/*
 * Prepares a new dm_task.
 */
struct dm_task *setup_dm_task(const char *name, int task);


/*
 * Query functions.  'name' is the device node name in the
 * device-mapper dir.
 */
int device_info(const char *name, struct dm_info *info);
int device_active(const char *name);
int device_suspended(const char *name);
int device_open_count(const char *name);


/*
 * Functions to manipulate an already active device.
 */
int device_deactivate(const char *name);
int device_suspend(const char *name);
int device_resume(const char *name);


/*
 * The next three functions populate a dm_task with a suitable
 * set of targets.
 */

/*
 * Creates a device with a mapping table as specified by the lv.
 */
int device_populate_lv(struct dm_task *dmt, struct logical_volume *lv);

/*
 * Layers the origin device above an already active 'real'
 * device.
 */
int device_populate_origin(struct dm_task *dmt, struct logical_volume *lv,
			   const char *real);

/*
 * Creates a snapshot device for a given origin and exception
 * storage area.
 */
int device_populate_snapshot(struct dm_task *dmt, struct logical_volume *lv,
			     const char *origin, const char *cow_device);

#endif
