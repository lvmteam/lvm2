/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 *
 */

#ifndef _LVM_XLATE_H
#define _LVM_XLATE_H

#include <asm/byteorder.h>

#define xlate16(x) __cpu_to_le16((x))
#define xlate32(x) __cpu_to_le32((x))
#define xlate64(x) __cpu_to_le64((x))

#endif
