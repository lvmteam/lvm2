/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_XLATE_H
#define _LVM_XLATE_H

#ifdef linux
#  include <asm/byteorder.h>
#  define xlate16(x) __cpu_to_le16((x))
#  define xlate32(x) __cpu_to_le32((x))
#  define xlate64(x) __cpu_to_le64((x))
#  define xlate16_be(x) __cpu_to_be16((x))
#  define xlate32_be(x) __cpu_to_be32((x))
#  define xlate64_be(x) __cpu_to_be64((x))
#else
#  include <machine/endian.h>
#  if !defined(BYTE_ORDER) || \
      (BYTE_ORDER != BIG_ENDIAN && BYTE_ORDER != LITTLE_ENDIAN)
#    error "Undefined or unrecognised BYTE_ORDER";
#  endif
#  define __xlate16(x) (((x) & 0x00ffU) << 8 | \
			((x) & 0xff00U) >> 8)
#  define __xlate32(x) (((x) & 0x000000ffU) << 24 | \
			((x) & 0xff000000U) >> 24 | \
			((x) & 0x0000ff00U) << 8  | \
			((x) & 0x00ff0000U) >> 8)
#  define __xlate64(x) (((x) & 0x00000000000000ffU) << 56 | \
			((x) & 0xff00000000000000U) >> 56 | \
			((x) & 0x000000000000ff00U) << 40 | \
			((x) & 0x00ff000000000000U) >> 40 | \
			((x) & 0x0000000000ff0000U) << 24 | \
			((x) & 0x0000ff0000000000U) >> 24 | \
			((x) & 0x00000000ff000000U) << 8 | \
			((x) & 0x000000ff00000000U) >> 8)
#  if BYTE_ORDER == LITTLE_ENDIAN
#    define xlate16(x) (x)
#    define xlate32(x) (x)
#    define xlate64(x) (x)
#    define xlate16_be(x) __xlate16(x)
#    define xlate32_be(x) __xlate32(x)
#    define xlate64_be(x) __xlate64(x)
#  else
#    define xlate16(x) __xlate16(x)
#    define xlate32(x) __xlate32(x)
#    define xlate64(x) __xlate64(x)
#    define xlate16_be(x) (x)
#    define xlate32_be(x) (x)
#    define xlate64_be(x) (x)
#  endif
#endif

#endif
