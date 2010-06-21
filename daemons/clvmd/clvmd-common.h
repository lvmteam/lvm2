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
 * This file must be included first by every clvmd source file.
 */
#ifndef _LVM_CLVMD_COMMON_H
#define _LVM_CLVMD_COMMON_H

#include "configure.h"

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "libdevmapper.h"

#include "lvm-logging.h"

#include <unistd.h>
#include <sys/stat.h>

#endif
