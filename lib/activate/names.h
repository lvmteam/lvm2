/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_NAMES_H
#define _LVM_NAMES_H

#include <stdlib.h>

/*
 * Functions that build up useful paths to devices, sym-links
 * etc.  Names are passed in as strings, rather than via the
 * appropriate metadata structures, so we can use it for renaming
 * devices.
 */


/*
 * The name of the device-mapper device for a particular LV.
 * eg, vg0:music
 */
int build_dm_name(char *buffer, size_t len, const char *prefix,
		  const char *vg_name, const char *lv_name);

/*
 * The path of the device-mapper device for a particular LV.
 * eg, /dev/device-mapper/vg0:music
 */
int build_dm_path(char *buffer, size_t len, const char *prefix,
		  const char *vg_name, const char *lv_name);

/*
 * Path to the volume group directory.
 * eg, /dev/vg0
 */
int build_vg_path(char *buffer, size_t len,
		  const char *dev_dir, const char *vg_name);

/*
 * Path to the symbolic link that lives in the volume group
 * directory.
 * eg, /dev/vg0/music
 */
int build_lv_link_path(char *buffer, size_t len,
		       const char *dev_dir,
		       const char *vg_name, const char *lv_name);

#endif
