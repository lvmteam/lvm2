/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_DEV_MANAGER_H
#define _LVM_DEV_MANAGER_H

#include "metadata.h"

#include <libdevmapper.h>

struct dev_manager;

/*
 * Constructor and destructor.
 */
struct dev_manager *dev_manager_create(const char *vg_name);
void dev_manager_destroy(struct dev_manager *dm);

/*
 * The device handler is responsible for creating all the layered
 * dm devices, and ensuring that all constraints are maintained
 * (eg, an origin is created before its snapshot, but is not
 * unsuspended until the snapshot is also created.)
 */
int dev_manager_info(struct dev_manager *dm, struct logical_volume *lv,
		     struct dm_info *info);
int dev_manager_get_snapshot_use(struct dev_manager *dm, 
 		                   struct logical_volume *lv, float *percent);
int dev_manager_suspend(struct dev_manager *dm, struct logical_volume *lv);
int dev_manager_activate(struct dev_manager *dm, struct logical_volume *lv);
int dev_manager_deactivate(struct dev_manager *dm, struct logical_volume *lv);


/*
 * Put the desired changes into effect.
 */
int dev_manager_execute(struct dev_manager *dm);

#endif
