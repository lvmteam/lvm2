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
 * The backup format is used to maintain a set of backup files.
 * 'retain_days' gives the minimum number of days that a backup must 
 *               be held for.
 *
 * 'min_backups' is the minimum number of backups required for each volume
 *               group.
 */
struct format_instance *backup_format_create(struct cmd_context *cmd,
					     const char *dir,
					     uint32_t retain_days,
					     uint32_t min_backups);

void backup_expire(struct format_instance *fi);

/*
 * The text format can read and write a volume_group to a file.
 */
struct format_instance *text_format_create(struct cmd_context *cmd,
					   const char *file);

#endif
