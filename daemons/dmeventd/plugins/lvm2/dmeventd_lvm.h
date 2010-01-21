/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
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

/*
 * Wrappers around liblvm2cmd functions for dmeventd plug-ins.
 *
 * liblvm2cmd is not thread-safe so the locking in this library helps dmeventd
 * threads to co-operate in sharing a single instance.
 *
 * FIXME Either support this properly as a generic liblvm2cmd wrapper or make
 * liblvm2cmd thread-safe so this can go away.
 */

#include "libdevmapper.h"

#ifndef _DMEVENTD_LVMWRAP_H
#define _DMEVENTD_LVMWRAP_H

int dmeventd_lvm2_init(void);
void dmeventd_lvm2_exit(void);
int dmeventd_lvm2_run(const char *cmdline);

void dmeventd_lvm2_lock(void);
void dmeventd_lvm2_unlock(void);

struct dm_pool *dmeventd_lvm2_pool(void);

#endif /* _DMEVENTD_LVMWRAP_H */
