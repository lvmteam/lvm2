/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 *
 */

#ifndef _LVM_XLATE_H
#define _LVM_XLATE_H

#include <asm/byteorder.h>

static inline uint16_t xlate16(uint16_t x)
{
	return __cpu_to_le16(x);
}

static inline uint32_t xlate32(uint32_t x)
{
	return __cpu_to_le32(x);
}

static inline uint64_t xlate64(uint64_t x)
{
	return __cpu_to_le64(x);
}

#endif
