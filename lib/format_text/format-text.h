/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_FORMAT_TEXT_H
#define _LVM_FORMAT_TEXT_H

#include "lvm-types.h"
#include "metadata.h"

struct format_instance *backup_format_create(struct cmd_context *cmd,
					     const char *dir,
					     uint32_t retain_days,
					     uint32_t min_backups);

void backup_expire(struct format_instance *fi);

#endif
