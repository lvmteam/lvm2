/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_FS_H
#define _LVM_FS_H

#include "metadata.h"

/*
 * These calls, private to the activate unit, set
 * up the volume group directory in /dev and the
 * symbolic links to the dm device.
 */
int fs_add_lv(struct logical_volume *lv, const char *dev);
int fs_del_lv(struct logical_volume *lv);
int fs_rename_lv(struct logical_volume *lv,
		 const char *dev, const char *old_name);
void fs_unlock(void);

#endif
