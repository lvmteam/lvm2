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
	int ret;
	
	if (!md_filtering())
		return 1;
	
	ret = dev_is_md(dev, NULL);

	if (ret == 1) {
		log_debug("%s: Skipping md component device", dev_name(dev));
		return 0;
	}

	if (ret < 0) {
		log_debug("%s: Skipping: error in md component detection");
		return 0;
	}

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
