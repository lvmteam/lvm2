/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 *
 */

#ifndef _LVM_XLATE_H
#define _LVM_XLATE_H

#include <stdlib.h>
#include <stdint.h>

#if __BYTE_ORDER == __BIG_ENDIAN

static inline uint16_t swab16(uint16_t x)
{
	return (x & 0x00ffU) << 8 |
			(x & 0xff00U) >> 8;
}

static inline uint32_t swab32(uint32_t x)
{
	return (x & 0x000000ffU) << 24 |
			(x & 0x0000ff00U) << 8 |
			(x & 0x00ff0000U) >> 8 |
			(x & 0xff000000U) >> 24;
}

static inline uint64_t swab64(uint64_t x)
{
	return (x & 0x00000000000000ffULL) << 56 |
			(x & 0x000000000000ff00ULL) << 40 |
			(x & 0x0000000000ff0000ULL) << 24 |
			(x & 0x00000000ff000000ULL) << 8 |
			(x & 0x000000ff00000000ULL) >> 8 |
			(x & 0x0000ff0000000000ULL) >> 24 |
			(x & 0x00ff000000000000ULL) >> 40 |
			(x & 0xff00000000000000ULL) >> 56;
}

#   define xlate16(x) swab16((x))
#   define xlate32(x) swab32((x))
#   define xlate64(x) swab64((x))
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#   define xlate16(x) (x)
#   define xlate32(x) (x)
#   define xlate64(x) (x)
#else
#   error "__BYTE_ORDER must be defined as __LITTLE_ENDIAN or __BIG_ENDIAN"
#endif

#endif
