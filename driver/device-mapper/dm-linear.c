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
		      char *args, void **context)
{
	struct linear_c *lc;
	unsigned int start;
	int r = -EINVAL;
	char *tok;
	char *path;
	char *p = args;

	*context = "No device path given";
	path = next_token(&p);
	if (!path)
		goto bad;

	*context = "No initial offset given";
	tok = next_token(&p);
	if (!tok)
		goto bad;
	start = simple_strtoul(tok, NULL, 10);

	*context = "Cannot allocate linear context private structure";
	lc = kmalloc(sizeof(lc), GFP_KERNEL);
	if (lc == NULL)
		goto bad;

	*context = "Cannot get target device";
	r = dm_table_get_device(t, path, &lc->dev);
	if (r)
		goto bad_free;

	lc->delta = (int) start - (int) b;
	*context = lc;
	return 0;

bad_free:
	kfree(lc);
bad:
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

/*
 * Debugging use only.
 */
static char *linear_print(void *context)
{
	struct linear_c *lc = (struct linear_c *)context;
static char buf[256];
	sprintf(buf, " %lu", lc->delta);
	return buf;
}

static struct target_type linear_target = {
	name: "linear",
	module: THIS_MODULE,
	ctr: linear_ctr,
	dtr: linear_dtr,
	map: linear_map,
	print: linear_print,
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
MODULE_LICENSE("GPL");

