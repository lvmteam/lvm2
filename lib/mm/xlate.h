/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 *
 */

#ifndef _LVM_XLATE_H
#define _LVM_XLATE_H

#ifdef linux
#  include <asm/byteorder.h>
#  define xlate16(x) __cpu_to_le16((x))
#  define xlate32(x) __cpu_to_le32((x))
#  define xlate64(x) __cpu_to_le64((x))
#else
#  include <machine/endian.h>
#  if !defined(BYTE_ORDER) || \
      (BYTE_ORDER != BIG_ENDIAN && BYTE_ORDER != LITTLE_ENDIAN)
#    error "Undefined or unrecognised BYTE_ORDER";
#  endif
#  if BYTE_ORDER == LITTLE_ENDIAN
#    define xlate16(x) (x)
#    define xlate32(x) (x)
#    define xlate64(x) (x)
#  else
#    define xlate16(x) (((x) & 0x00ffU) << 8 | \
		        ((x) & 0xff00U) >> 8)
#    define xlate32(x) (((x) & 0x000000ffU) << 24 | \
		        ((x) & 0xff000000U) >> 24 | \
		        ((x) & 0x0000ff00U) << 8  | \
		        ((x) & 0x00ff0000U) >> 8)
#    define xlate64(x) (((x) & 0x00000000000000ffU) << 56 | \
                        ((x) & 0xff00000000000000U) >> 56 | \
                        ((x) & 0x000000000000ff00U) << 40 | \
                        ((x) & 0x00ff000000000000U) >> 40 | \
                        ((x) & 0x0000000000ff0000U) << 24 | \
                        ((x) & 0x0000ff0000000000U) >> 24 | \
                        ((x) & 0x00000000ff000000U) << 8 | \
                        ((x) & 0x000000ff00000000U) >> 8)
#  endif
#endif

#endif
