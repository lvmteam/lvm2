/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_FORMAT_TEXT_H
#define _LVM_FORMAT_TEXT_H

#include "lvm-types.h"
#include "metadata.h"
#include "uuid-map.h"

/*
 * Archives a vg config.  'retain_days' is the minimum number of
 * days that an archive file must be held for.  'min_archives' is
 * the minimum number of archives required to be kept for each
 * volume group.
 */
int archive_vg(struct volume_group *vg,
	       const char *dir,
	       const char *desc,
	       uint32_t retain_days,
	       uint32_t min_archive);


/*
 * The text format can read and write a volume_group to a file.
 */
struct format_instance *text_format_create(struct cmd_context *cmd,
					   const char *file,
					   struct uuid_map *um,
					   const char *desc);

#endif
