/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * This file must be included first by every library source file.
 */
#ifndef _LVM_LIB_H
#define _LVM_LIB_H

#include "device_mapper/all.h"
#include "base/memory/zalloc.h"
#include "lib/misc/intl.h"
#include "lib/misc/util.h"

#ifdef DM
#  include "libdm/misc/dm-logging.h"
#else
#  include "lib/log/lvm-logging.h"
#  include "lib/misc/lvm-globals.h"
#  include "lib/misc/lvm-wrappers.h"
#  include "lib/misc/lvm-maths.h"
#endif

#include <unistd.h>

#endif
