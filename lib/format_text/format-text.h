/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_FORMAT_TEXT_H
#define _LVM_FORMAT_TEXT_H

#include "lvm-types.h"
#include "metadata.h"

/*
 * The archive format is used to maintain a set of metadata backup files
 * in an archive directory.
 * 'retain_days' is the minimum number of days that an archive file must 
 *               be held for.
 *
 * 'min_archives' is the minimum number of archives required to be kept
 *               for each volume group.
 */
struct format_instance *archive_format_create(struct cmd_context *cmd,
					     const char *dir,
					     uint32_t retain_days,
					     uint32_t min_archives);

void backup_expire(struct format_instance *fi);

/*
 * The text format can read and write a volume_group to a file.
 */
struct format_instance *text_format_create(struct cmd_context *cmd,
					   const char *file);

#endif
