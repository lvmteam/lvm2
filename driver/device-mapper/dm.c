/*
 * device-mapper.c
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

/*
 * Changelog
 *
 *    14/08/2001 - First Version [Joe Thornber]
 */


/* TODO:
 *
 * dm_ctr_fn should provide the sector sizes, and hardsector_sizes set
 * to the smallest of these.
 */

#include <linux/version.h>
#include <linux/major.h>
#include <linux/iobuf.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/compatmac.h>
#include <linux/cache.h>
#include <linux/device-mapper.h>

/* defines for blk.h */
#define MAJOR_NR DM_BLK_MAJOR
#define DEVICE_OFF(device)
#define LOCAL_END_REQUEST

#include <linux/blk.h>

/*
 * This driver attempts to provide a generic way of specifying logical
 * devices which are mapped onto other devices.
 *
 * It does this by mapping sections of the logical device onto 'targets'.
 *
 * When the logical device is accessed the make_request function looks up
 * the correct target for the given sector, and then asks this target
 * to do the remapping.
 *
 * A btree like structure is used to hold the sector range -> target
 * mapping.  Because we know all the entries in the btree in advance
 * we can make a very compact tree, omitting pointers to child nodes,
 * (child nodes locations can be calculated). Each node of the btree is
 * 1 level cache line in size, this gives a small performance boost.
 *
 * A userland test program for the btree gave the following results on a
 * 1 Gigahertz Athlon machine:
 *
 * entries in btree               lookups per second
 * ----------------               ------------------
 * 5                              25,000,000
 * 1000                           7,700,000
 * 10,000,000                     3,800,000
 *
 * Of course these results should be taken with a pinch of salt; the lookups
 * were sequential and there were no other applications (other than X + emacs)
 * running to give any pressure on the level 1 cache.
 *
 * Typically LVM users would find they have very few targets for each
 * LV (probably less than 10).
 *
 * Target types are not hard coded, instead the
 * register_mapping_type function should be called.  A target type
 * is specified using three functions (see the header):
 *
 * dm_ctr_fn - takes a string and contructs a target specific piece of
 *             context data.
 * dm_dtr_fn - destroy contexts.
 * dm_map_fn - function that takes a buffer_head and some previously
 *             constructed context and performs the remapping.
 *
 * This file contains two trivial mappers, which are automatically
 * registered: 'linear', and 'io_error'.  Linear alone is enough to
 * implement most LVM features (omitting striped volumes and
 * snapshots).
 *
 * The driver is controlled through a /proc interface...
 * FIXME: finish
 *
 * At the moment the table assumes 32 bit keys (sectors), the move to
 * 64 bits will involve no interface changes, since the tables will be
 * read in as ascii data.  A different table implementation can
 * therefor be provided at another time.  Either just by changing offset_t
 * to 64 bits, or maybe implementing a structure which looks up the keys in
 * stages (ie, 32 bits at a time).
 *
 * More interesting targets:
 *
 * striped mapping; given a stripe size and a number of device regions
 * this would stripe data across the regions.  Especially useful, since
 * we could limit each striped region to a 32 bit area and then avoid
 * nasy 64 bit %'s.
 *
 * mirror mapping (reflector ?); would set off a kernel thread slowly
 * copying data from one region to another, ensuring that any new
 * writes got copied to both destinations correctly.  Great for
 * implementing pvmove.  Not sure how userland would be notified that
 * the copying process had completed.  Possibly by reading a /proc entry
 * for the LV. Could also use poll() for this kind of thing.
 */

#include "dm.h"

#define MAX_DEVICES 64
#define DEFAULT_READ_AHEAD 64

#define WARN(f, x...) printk(KERN_WARNING "%s " f "\n", _name , ## x)

const char *_name = "device-mapper";
int _version[3] = {1, 0, 0};

#define rl down_read(&_dev_lock)
#define ru up_read(&_dev_lock)
#define wl down_write(&_dev_lock)
#define wu up_read(&_dev_lock)

struct rw_semaphore _dev_lock;
static struct mapped_device *_devs[MAX_DEVICES];

/* block device arrays */
static int _block_size[MAX_DEVICES];
static int _blksize_size[MAX_DEVICES];
static int _hardsect_size[MAX_DEVICES];

static int _blk_open(struct inode *inode, struct file *file);
static int _blk_close(struct inode *inode, struct file *file);
static int _blk_ioctl(struct inode *inode, struct file *file,
		      uint command, ulong a);

static struct block_device_operations _blk_dops = {
	open:     _blk_open,
	release:  _blk_close,
	ioctl:    _blk_ioctl
};

static int _request_fn(request_queue_t *q, int rw, struct buffer_head *bh);

/*
 * setup and teardown the driver
 */
static int _init(void)
{
	init_rwsem(&_dev_lock);

	if (!dm_std_targets())
		return -EIO;	/* FIXME: better error value */

	/* set up the arrays */
	read_ahead[MAJOR_NR] = DEFAULT_READ_AHEAD;
	blk_size[MAJOR_NR] = _block_size;
	blksize_size[MAJOR_NR] = _blksize_size;
	hardsect_size[MAJOR_NR] = _hardsect_size;

	if (register_blkdev(MAJOR_NR, _name, &_blk_dops) < 0) {
		printk(KERN_ERR "%s -- register_blkdev failed\n", _name);
		return -EIO;
	}

	blk_queue_make_request(BLK_DEFAULT_QUEUE(MAJOR_NR), _request_fn);

	printk(KERN_INFO "%s(%d, %d, %d) successfully initialised\n", _name,
	       _version[0], _version[1], _version[2]);
	return 0;
}

static void _fin(void)
{
	if (unregister_blkdev(MAJOR_NR, _name) < 0)
		printk(KERN_ERR "%s -- unregister_blkdev failed\n", _name);

	read_ahead[MAJOR_NR] = 0;
	blk_size[MAJOR_NR] = 0;
	blksize_size[MAJOR_NR] = 0;
	hardsect_size[MAJOR_NR] = 0;

	printk(KERN_INFO "%s(%d, %d, %d) successfully finalised\n", _name,
	       _version[0], _version[1], _version[2]);
}

/*
 * block device functions
 */
static int _blk_open(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	wl;
	md = _devs[minor];

	if (!md || !is_active(md)) {
		wu;
		return -ENXIO;
	}

	md->use_count++;
	wu;

	MOD_INC_USE_COUNT;
	return 0;
}

static int _blk_close(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	wl;
	md = _devs[minor];
	if (!md || md->use_count <= 1) {
		WARN("reference count in mapped_device incorrect");
		wu;
		return -ENXIO;
	}

	md->use_count--;
	wu;

	MOD_DEC_USE_COUNT;
	return 0;
}

static int _blk_ioctl(struct inode *inode, struct file *file,
		      uint command, ulong a)
{
	/* FIXME: check in the latest Rubini that all expected ioctl's
	   are supported */

	int minor = MINOR(inode->i_rdev);
	long size;

	switch (command) {
	case BLKGETSIZE:
		size = _block_size[minor] * 1024 / _hardsect_size[minor];
		if (copy_to_user((void *) a, &size, sizeof(long)))
			return -EFAULT;
		break;

	case BLKFLSBUF:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		fsync_dev(inode->i_rdev);
		invalidate_buffers(inode->i_rdev);
		return 0;

	case BLKRAGET:
		if (copy_to_user((void *) a, &read_ahead[MAJOR(inode->i_rdev)],
				sizeof(long)))
			return -EFAULT;
		return 0;

	case BLKRASET:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		read_ahead[MAJOR(inode->i_rdev)] = a;
		return 0;

	case BLKRRPART:
		return -EINVAL;

	default:
		printk(KERN_WARNING "%s - unknown block ioctl %d",
		       _name, command);
		return -EINVAL;
	}

	return 0;
}

static int _request_fn(request_queue_t *q, int rw, struct buffer_head *bh)
{
	struct mapped_device *md;
	offset_t *node;
	int i = 0, l, next_node = 0, ret = 0;
	int minor = MINOR(bh->b_rdev);
	dm_map_fn fn;
	void *context;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	rl;
	md = _devs[minor];

	if (!md) {
		ret = -ENXIO;
		goto out;
	}

	for (l = 0; l < md->depth; l++) {
		next_node = ((KEYS_PER_NODE + 1) * next_node) + i;
		node = md->index[l] + (next_node * KEYS_PER_NODE);

		for (i = 0; i < KEYS_PER_NODE; i++)
			if (node[i] >= bh->b_rsector)
				break;
	}

	next_node = (KEYS_PER_NODE * next_node) + i;
	fn = md->targets[next_node];
	context = md->contexts[next_node];

	if (fn) {
		if ((ret = fn(bh, context)))
			atomic_inc(&md->pending);
	} else
		buffer_IO_error(bh);

 out:
	ru;
	return ret;
}

static inline int __specific_dev(int minor)
{
	if (minor > MAX_DEVICES) {
		WARN("request for a mapped_device > than MAX_DEVICES");
		return 0;
	}

	if (!_devs[minor])
		return minor;

	return -1;
}

static inline int __any_old_dev(void)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++)
		if (!_devs[i])
			return i;

	return -1;
}

static struct mapped_device *_alloc_dev(int minor)
{
	struct mapped_device *md = kmalloc(sizeof(*md), GFP_KERNEL);

	wl;
	minor = (minor < 0) ? __any_old_dev() : __specific_dev(minor);

	if (minor < 0) {
		WARN("no free devices available");
		wu;
		kfree(md);
		return 0;
	}

	md->dev = MKDEV(DM_BLK_MAJOR, minor);
	md->name[0] = '\0';
	md->state = 0;

	_devs[minor] = md;
	wu;

	return md;
}

static void _free_dev(struct mapped_device *md)
{
	int minor = MINOR(md->dev);

	wl;
	_devs[minor] = 0;
	wu;

	kfree(md);
}

static inline struct mapped_device *__find_name(const char *name)
{
	int i;
	for (i = 0; i < MAX_DEVICES; i++)
		if (_devs[i] && !strcmp(_devs[i]->name, name))
			return _devs[i];

	return 0;
}

static int _open_dev(struct dev_list *d)
{
	int err;

	if (!(d->bd = bdget(kdev_t_to_nr(d->dev))))
		return -ENOMEM;

	if ((err = blkdev_get(d->bd, FMODE_READ|FMODE_WRITE, 0, BDEV_FILE))) {
		bdput(d->bd);
		return err;
	}

	return 0;
}

static void _close_dev(struct dev_list *d)
{
	blkdev_put(d->bd, BDEV_FILE);
	bdput(d->bd);
	d->bd = 0;
}

struct mapped_device *dm_find_name(const char *name)
{
	struct mapped_device *md;

	rl;
	md = __find_name(name);
	ru;

	return md;
}

struct mapped_device *dm_find_minor(int minor)
{
	struct mapped_device *md;

	rl;
	md = _devs[minor];
	ru;

	return md;
}

int dm_create(int minor, const char *name)
{
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	if (!(md = _alloc_dev(minor)))
		return -ENOMEM;

	wl;
	if (__find_name(name)) {
		WARN("device with that name already exists");
		wu;
		_free_dev(md);
		return -EINVAL;
	}

	strcpy(md->name, name);
	_devs[minor] = md;
	wu;

	return 0;
}

int dm_remove(const char *name, int minor)
{
	struct mapped_device *md;
	struct dev_list *d, *n;

	wl;
	if (!(md = __find_name(name))) {
		wu;
		return -ENXIO;
	}

	dm_clear_table(md);
	for (d = md->devices; d; d = n) {
		n = d->next;
		kfree(d);
	}

	minor = MINOR(md->dev);
	_free_dev(md);
	_devs[minor] = 0;
	wu;

	return 0;
}

int dm_add_device(struct mapped_device *md, kdev_t dev)
{
	struct dev_list *d = kmalloc(sizeof(*d), GFP_KERNEL);

	if (!d)
		return 0;

	d->dev = dev;
	d->next = md->devices;
	md->devices = d;

	return 1;
}

int dm_activate(struct mapped_device *md)
{
	int ret;
	struct dev_list *d, *od;

	if (is_active(md))
		return 1;

	rl;
	/* open all the devices */
	for (d = md->devices; d; d = d->next)
		if ((ret = _open_dev(d)))
			goto bad;
	ru;

	return 0;

 bad:

	od = d;
	for (d = md->devices; d != od; d = d->next)
		_close_dev(d);
	ru;

	return ret;
}

void dm_suspend(struct mapped_device *md)
{
	struct dev_list *d;
	if (!is_active(md))
		return;

	/* close all the devices */
	for (d = md->devices; d; d = d->next)
		_close_dev(d);

	set_active(md, 0);
}


/*
 * module hooks
 */
module_init(_init);
module_exit(_fin);

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
