/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2025 Red Hat, Inc. All rights reserved.
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
 * This header provides backward compatibility for endian conversion macros.
 * Modern systems have these macros in <endian.h>, but this file ensures
 * compatibility with older glibc versions (< 2.9) and non-glibc systems.
 */

#ifndef _LVM_XLATE_H
#define _LVM_XLATE_H

#ifdef __linux__
#  include <endian.h>
#  include <byteswap.h>
#else
#  include <machine/endian.h>
#  define bswap_16(x) (((x) & 0x00ffU) << 8 | \
		       ((x) & 0xff00U) >> 8)
#  define bswap_32(x) (((x) & 0x000000ffU) << 24 | \
		       ((x) & 0xff000000U) >> 24 | \
		       ((x) & 0x0000ff00U) << 8  | \
		       ((x) & 0x00ff0000U) >> 8)
#  define bswap_64(x) (((x) & 0x00000000000000ffULL) << 56 | \
		       ((x) & 0xff00000000000000ULL) >> 56 | \
		       ((x) & 0x000000000000ff00ULL) << 40 | \
		       ((x) & 0x00ff000000000000ULL) >> 40 | \
		       ((x) & 0x0000000000ff0000ULL) << 24 | \
		       ((x) & 0x0000ff0000000000ULL) >> 24 | \
		       ((x) & 0x00000000ff000000ULL) << 8 | \
		       ((x) & 0x000000ff00000000ULL) >> 8)
#endif

/*
 * NOTE: This file may appear unused to static analysis tools like Coverity
 * on modern systems, but it is essential for backward compatibility.
 *
 * So to make it look as 'used' header file, for Coverity scan
 * lets redefine those macros.
 *
 * TODO: is there any better trick how to avoid HFA warnings and add
 * annotation comment before every inclusion of this header file ??
 */

#ifdef __COVERITY__

#  undef htobe16
#  undef htole16
#  undef be16toh
#  undef le16toh

#  undef htobe32
#  undef htole32
#  undef be32toh
#  undef le32toh

#  undef htobe64
#  undef htole64
#  undef be64toh
#  undef le64toh

#endif


#ifndef htobe16

# if BYTE_ORDER == LITTLE_ENDIAN

#  define htobe16(x) bswap_16 (x)
#  define htole16(x) (uint16_t) (x)
#  define be16toh(x) bswap_16 (x)
#  define le16toh(x) (uint16_t) (x)

#  define htobe32(x) bswap_32 (x)
#  define htole32(x) (uint32_t) (x)
#  define be32toh(x) bswap_32 (x)
#  define le32toh(x) (uint32_t) (x)

#  define htobe64(x) bswap_64 (x)
#  define htole64(x) (uint64_t) (x)
#  define be64toh(x) bswap_64 (x)
#  define le64toh(x) (uint64_t) (x)

# else

#  define htobe16(x) (uint16_t) (x)
#  define htole16(x) bswap_16 (x)
#  define be16toh(x) (uint16_t) (x)
#  define le16toh(x) bswap_16 (x)

#  define htobe32(x) (uint32_t) (x)
#  define htole32(x) bswap_32 (x)
#  define be32toh(x) (uint32_t) (x)
#  define le32toh(x) bswap_32 (x)

#  define htobe64(x) (uint64_t) (x)
#  define htole64(x) bswap_64 (x)
#  define be64toh(x) (uint64_t) (x)
#  define le64toh(x) bswap_64 (x)

# endif

#endif

#endif
