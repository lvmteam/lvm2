/*
 * Copyright (C) 2004 Luca Berra
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
#include "filter-md.h"
#include "metadata.h"

#ifdef linux

/* Lifted from <linux/raid/md_p.h> because of difficulty including it */

#define MD_SB_MAGIC 0xa92b4efc
#define MD_RESERVED_BYTES (64 * 1024)
#define MD_RESERVED_SECTORS (MD_RESERVED_BYTES / 512)
#define MD_NEW_SIZE_SECTORS(x) ((x & ~(MD_RESERVED_SECTORS - 1)) \
				- MD_RESERVED_SECTORS)

static int _ignore_md(struct dev_filter *f, struct device *dev)
{
	uint64_t size, sector;
	uint32_t md_magic;

	if (!dev_get_size(dev, &size)) {
		stack;
		return 0;
	}

	if (size < MD_RESERVED_SECTORS * 2)
		/*
		 * We could ignore it since it is obviously too
		 * small, but that's not our job.
		 */
		return 1;

	if (!dev_open(dev)) {
		stack;
		return 0;
	}

	sector = MD_NEW_SIZE_SECTORS(size);

	/* Check if it is an md component device. */
	if (dev_read(dev, sector << SECTOR_SHIFT, sizeof(uint32_t), &md_magic)) {
		if (md_magic == MD_SB_MAGIC) {
			log_debug("%s: Skipping md component device",
				  dev_name(dev));
			if (!dev_close(dev))
				stack;
			return 0;
		}
	}

	if (!dev_close(dev))
		stack;

	return 1;
}

static void _destroy(struct dev_filter *f)
{
	dbg_free(f);
}

struct dev_filter *md_filter_create(void)
{
	struct dev_filter *f;

	if (!(f = dbg_malloc(sizeof(*f)))) {
		log_error("md filter allocation failed");
		return NULL;
	}

	f->passes_filter = _ignore_md;
	f->destroy = _destroy;
	f->private = NULL;

	return f;
}

#else

struct dev_filter *md_filter_create(void)
{
	return NULL;
}

#endif
