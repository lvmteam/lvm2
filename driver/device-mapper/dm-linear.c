/*
 * dm-linear.c
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This software is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device-mapper.h>

#include "dm.h" /* going away soon */

/*
 * linear: maps a linear range of a device.
 */
struct linear_c {
	kdev_t dev;
	long delta;		/* FIXME: we need a signed offset type */
};

/*
 * construct a linear mapping.
 * <dev_path> <offset>
 */
static int linear_ctr(struct dm_table *t, offset_t b, offset_t l,
		      struct text_region *args, void **result,
		      dm_error_fn fn, void *private)
{
	struct linear_c *lc;
	unsigned int start;
	kdev_t dev;
	int r;
	char path[256];
	struct text_region word;

	if (!dm_get_word(args, &word)) {
		fn("couldn't get device path", private);
		return -EINVAL;
	}

	dm_txt_copy(path, sizeof(path) - 1, &word);

	if ((r = dm_table_lookup_device(path, &dev))) {
		fn("no such device", private);
		return r;
	}

	if (!dm_get_number(args, &start)) {
		fn("destination start not given", private);
		return -EINVAL;
	}

	if (!(lc = kmalloc(sizeof(lc), GFP_KERNEL))) {
		fn("couldn't allocate memory for linear context\n", private);
		return -ENOMEM;
	}

	lc->dev = dev;
	lc->delta = (int) start - (int) b;

	if ((r = dm_table_add_device(t, lc->dev))) {
		fn("failed to add destination device to list", private);
		kfree(lc);
		return r;
	}

	*result = lc;
	return 0;
}

static void linear_dtr(struct dm_table *t, void *c)
{
	struct linear_c *lc = (struct linear_c *) c;
	dm_table_remove_device(t, lc->dev);
	kfree(c);
}

static int linear_map(struct buffer_head *bh, void *context)
{
	struct linear_c *lc = (struct linear_c *) context;

	bh->b_rdev = lc->dev;
	bh->b_rsector = bh->b_rsector + lc->delta;
	return 1;
}

static struct target_type linear_target = {
	name: "linear",
	module: &THIS_MODULE,
	ctr: linear_ctr,
	dtr: linear_dtr,
	map: linear_map,
};

static int __init linear_init(void)
{
	int rv;

	rv = dm_register_target(&linear_target);
	if (rv < 0) {
		printk(KERN_ERR "Device mapper: Linear: register failed %d\n", rv);
	}

	return rv;
}

static void __exit linear_exit(void)
{
	int rv;

	rv = dm_unregister_target(&linear_target);

	if (rv < 0) {
		printk(KERN_ERR "Device mapper: Linear: unregister failed %d\n", rv);
	}
}

module_init(linear_init);
module_exit(linear_exit);

MODULE_AUTHOR("Joe Thornber <thornber@uk.sistina.com>");
MODULE_DESCRIPTION("Device Mapper: Linear mapping");

