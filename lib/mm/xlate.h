/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 *
 */

#ifndef _LVM_XLATE_H
#define _LVM_XLATE_H

/* FIXME: finish these as inlines */

uint16_t shuffle16(uint16_t n);
uint32_t shuffle32(uint32_t n);
uint64_t shuffle64(uint64_t n);

/* xlate functions move data between core and disk */
#if __BYTE_ORDER == __BIG_ENDIAN
#   define xlate16(x) shuffle16(x)
#   define xlate32(x) shuffle32(x)
#   define xlate64(x) shuffle64(x)

#elif __BYTE_ORDER == __LITTLE_ENDIAN
#   define xlate16(x) (x)
#   define xlate32(x) (x)
#   define xlate64(x) (x)

#else
#   error "__BYTE_ORDER must be defined as __LITTLE_ENDIAN or __BIG_ENDIAN"
#endif

#endif
