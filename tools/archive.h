/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_TOOL_ARCHIVE_H
#define _LVM_TOOL_ARCHIVE_H

#include "metadata.h"

/*
 * There are two operations that come under the
 * general area of backups.  'Archiving' occurs
 * just before a volume group configuration is
 * changed.  The user may configure when archived
 * files are expired.  Typically archives will be
 * stored in /etc/lvm/archive.
 *
 * A 'backup' is a redundant copy of the *current*
 * volume group configuration.  As such it should
 * be taken just after the volume group is
 * changed.  Only 1 backup file will exist.
 * Typically backups will be stored in
 * /etc/lvm/backups.
 */

int archive_init(const char *dir,
		 unsigned int keep_days, unsigned int keep_min);
void archive_exit(void);

void archive_enable(int flag);
int archive(struct volume_group *vg);

int backup_init(const char *dir);
void backup_exit(void);

void backup_enable(int flag);
int backup(struct volume_group *vg);

#endif
