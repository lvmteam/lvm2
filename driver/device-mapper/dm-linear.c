/*
 * dm-linear.c
 *
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

/*
 * linear: maps a linear range of a device.
 */
struct linear_c {
	long delta;		/* FIXME: we need a signed offset type */
	struct dm_dev *dev;
};

/*
 * construct a linear mapping.
 * <dev_path> <offset>
 */
static int linear_ctr(struct dm_table *t, offset_t b, offset_t l,
		      struct text_region *args, void **context,
		      dm_error_fn err, void *e_private)
{
	struct linear_c *lc;
	unsigned int start;
	struct text_region word;
	char path[256];		/* FIXME: magic */
	int r = -EINVAL;

	if (!(lc = kmalloc(sizeof(lc), GFP_KERNEL))) {
		err("couldn't allocate memory for linear context", e_private);
		return -ENOMEM;
	}

	if (!dm_get_word(args, &word)) {
		err("couldn't get device path", e_private);
		goto bad;
	}

	dm_txt_copy(path, sizeof(path) - 1, &word);

	if (!dm_get_number(args, &start)) {
		err("destination start not given", e_private);
		goto bad;
	}

	if ((r = dm_table_get_device(t, path, &lc->dev))) {
		err("couldn't lookup device", e_private);
		r = -ENXIO;
		goto bad;
	}

	lc->delta = (int) start - (int) b;
	*context = lc;
	return 0;

 bad:
	kfree(lc);
	return r;
}

static void linear_dtr(struct dm_table *t, void *c)
{
	struct linear_c *lc = (struct linear_c *) c;
	dm_table_put_device(t, lc->dev);
	kfree(c);
}

static int linear_map(struct buffer_head *bh, int rw, void *context)
{
	struct linear_c *lc = (struct linear_c *) context;

	bh->b_rdev = lc->dev->dev;
	bh->b_rsector = bh->b_rsector + lc->delta;
	return 1;
}

static struct target_type linear_target = {
	name: "linear",
	module: THIS_MODULE,
	ctr: linear_ctr,
	dtr: linear_dtr,
	map: linear_map,
};

static int __init linear_init(void)
{
	int r = dm_register_target(&linear_target);

	if (r < 0)
		printk(KERN_ERR
		       "Device mapper: Linear: register failed %d\n", r);

	return r;
}

static void __exit linear_exit(void)
{
	int r = dm_unregister_target(&linear_target);

	if (r < 0)
		printk(KERN_ERR
		       "Device mapper: Linear: unregister failed %d\n", r);
}

module_init(linear_init);
module_exit(linear_exit);

MODULE_AUTHOR("Joe Thornber <thornber@uk.sistina.com>");
MODULE_DESCRIPTION("Device Mapper: Linear mapping");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

