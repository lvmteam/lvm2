/*
 * Copyright (C) 2004 Luca Berra
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "metadata.h"
#include "xlate.h"

/* Lifted from <linux/raid/md_p.h> because of difficulty including it */

#define MD_SB_MAGIC 0xa92b4efc
#define MD_RESERVED_BYTES (64 * 1024)
#define MD_RESERVED_SECTORS (MD_RESERVED_BYTES / 512)
#define MD_NEW_SIZE_SECTORS(x) ((x & ~(MD_RESERVED_SECTORS - 1)) \
				- MD_RESERVED_SECTORS)

/*
 * Returns -1 on error
 */
int dev_is_md(struct device *dev, uint64_t *sb)
{
	int ret = 0;

#ifdef linux

	uint64_t size, sb_offset;
	uint32_t md_magic;

	if (!dev_get_size(dev, &size)) {
		stack;
		return -1;
	}

	if (size < MD_RESERVED_SECTORS * 2)
		return 0;

	if (!dev_open(dev)) {
		stack;
		return -1;
	}

	sb_offset = MD_NEW_SIZE_SECTORS(size) << SECTOR_SHIFT;

	/* Check if it is an md component device. */
	/* Version 1 is little endian; version 0.90.0 is machine endian */
	if (dev_read(dev, sb_offset, sizeof(uint32_t), &md_magic) &&
	    ((md_magic == xlate32(MD_SB_MAGIC)) ||
	     (md_magic == MD_SB_MAGIC))) {
		if (sb)
			*sb = sb_offset;
		ret = 1;
	}

	if (!dev_close(dev))
		stack;

#endif
	return ret;
}

