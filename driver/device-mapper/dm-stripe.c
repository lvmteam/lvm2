/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/device-mapper.h>

#include "dm.h"

struct stripe {
	struct dm_dev *dev;
	offset_t physical_start;
};

struct stripe_c {
	offset_t logical_start;
	uint32_t stripes;

	/* The size of this target / num. stripes */
	uint32_t stripe_width;

	/* eg, we stripe in 64k chunks */
	uint32_t chunk_shift;
	offset_t chunk_mask;

	struct stripe stripe[0];
};


static inline struct stripe_c *alloc_context(int stripes)
{
	size_t len = sizeof(struct stripe_c) +
		(sizeof(struct stripe) * stripes);
	return kmalloc(len, GFP_KERNEL);
}

/*
 * parses a single <dev> <sector> pair.
 */
static int get_stripe(struct dm_table *t, struct stripe_c *sc,
		      int stripe, char *args)
{
	int n, r;
	char path[256];		/* FIXME: buffer overrun risk */
	unsigned long start;

	if (sscanf(args, "%s %lu %n", path, &start, &n) != 2)
		return -EINVAL;

	if ((r = dm_table_get_device(t, path, start, sc->stripe_width,
				     &sc->stripe[stripe].dev)))
		return -ENXIO;

	sc->stripe[stripe].physical_start = start;
	return n;
}

/*
 * construct a striped mapping.
 * <number of stripes> <chunk size (2^^n)> [<dev_path> <offset>]+
 */
static int stripe_ctr(struct dm_table *t, offset_t b, offset_t l,
		      char *args, void **context)
{
	struct stripe_c *sc;
	uint32_t stripes;
	uint32_t chunk_size;
	int n, i;

	*context = "couldn't parse <stripes> <chunk size>";
	if (sscanf(args, "%u %u %n", &stripes, &chunk_size, &n) != 2) {
		return -EINVAL;
	}

	*context = "target length is not divisable by the number of stripes";
	if (l % stripes) {
		return -EINVAL;
	}

	*context = "couldn't allocate memory for striped context";
	if (!(sc = alloc_context(stripes))) {
		return -ENOMEM;
	}

	sc->logical_start = b;
	sc->stripes = stripes;
	sc->stripe_width = l / stripes;

	/*
	 * chunk_size is a power of two.  We only
	 * that power and the mask.
	 */
	*context = "invalid chunk size";
	if (!chunk_size) {
		return -EINVAL;
	}

	sc->chunk_mask = chunk_size - 1;
	for (sc->chunk_shift = 0; chunk_size; sc->chunk_shift++)
		chunk_size >>= 1;
	sc->chunk_shift--;

	/*
	 * Get the stripe destinations.
	 */
	for (i = 0; i < stripes; i++) {
		args += n;
		n = get_stripe(t, sc, i, args);

		*context = "couldn't parse stripe destination";
		if (n < 0) {
			kfree(sc);
			return n;
		}
	}


	*context = sc;
	return 0;
}

static void stripe_dtr(struct dm_table *t, void *c)
{
	unsigned int i;
	struct stripe_c *sc = (struct stripe_c *) c;

	for (i = 0; i < sc->stripes; i++)
		dm_table_put_device(t, sc->stripe[i].dev);

	kfree(sc);
}

static int stripe_map(struct buffer_head *bh, int rw, void *context)
{
	struct stripe_c *sc = (struct stripe_c *) context;

	offset_t offset = bh->b_rsector - sc->logical_start;
	uint32_t chunk = (uint32_t) (offset >> sc->chunk_shift);
	uint32_t stripe = chunk % sc->stripes; /* 32bit modulus */
	chunk = chunk / sc->stripes;

	bh->b_rdev = sc->stripe[stripe].dev->dev;
	bh->b_rsector = sc->stripe[stripe].physical_start +
		(chunk << sc->chunk_shift) +
		(offset & sc->chunk_mask);
	return 1;
}

static struct target_type stripe_target = {
	name: "striped",
	module: THIS_MODULE,
	ctr: stripe_ctr,
	dtr: stripe_dtr,
	map: stripe_map,
};

static int __init stripe_init(void)
{
	int r;

	if ((r = dm_register_target(&stripe_target)) < 0)
		WARN("linear target register failed");

	return r;
}

static void __exit stripe_exit(void)
{
	if (dm_unregister_target(&stripe_target))
		WARN("striped target unregister failed");
}

module_init(stripe_init);
module_exit(stripe_exit);

MODULE_AUTHOR("Joe Thornber <thornber@sistina.com>");
MODULE_DESCRIPTION("Device Mapper: Striped mapping");
MODULE_LICENSE("GPL");
