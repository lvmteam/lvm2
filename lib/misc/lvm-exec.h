/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_EXEC_H
#define _LVM_EXEC_H

#include "lib.h"

struct cmd_context;

/**
 * Execute command with paramaters and return status
 *
 * \param rstatus
 * Returns command's exit status code.
 *
 * \param sync_needed
 * Bool specifying whether local devices needs to be synchronized
 * before executing command.
 * Note: You cannot synchronize devices within activation context.
 *
 * \return
 * 0 (success) or -1 (failure).
 */
int exec_cmd(struct cmd_context *cmd, const char *const argv[],
	     int *rstatus, int sync_needed);

#endif
