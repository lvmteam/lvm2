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

#include "dm.h"

/* defines for blk.h */
#define MAJOR_NR DM_BLK_MAJOR
#define DEVICE_NR(device) MINOR(device)  /* has no partition bits */
#define DEVICE_NAME "device-mapper"      /* name for messaging */
#define DEVICE_NO_RANDOM                 /* no entropy to contribute */
#define DEVICE_OFF(d)                    /* do-nothing */

#include <linux/blk.h>

#define MAX_DEVICES 64
#define DEFAULT_READ_AHEAD 64

const char *_name = "device-mapper";
int _version[3] = {0, 1, 0};

struct io_hook {
	struct mapped_device *md;
	void (*end_io)(struct buffer_head *bh, int uptodate);
	void *context;
};

kmem_cache_t *_io_hook_cache;

struct rw_semaphore _dev_lock;
static struct mapped_device *_devs[MAX_DEVICES];

/* block device arrays */
static int _block_size[MAX_DEVICES];
static int _blksize_size[MAX_DEVICES];
static int _hardsect_size[MAX_DEVICES];

const char *_fs_dir = "device-mapper";
static devfs_handle_t _dev_dir;

static int request(request_queue_t *q, int rw, struct buffer_head *bh);

/*
 * setup and teardown the driver
 */
static int dm_init(void)
{
	int ret;

	init_rwsem(&_dev_lock);

	if (!(_io_hook_cache =
	      kmem_cache_create("dm io hooks", sizeof(struct io_hook),
				0, 0, NULL, NULL)))
		return -ENOMEM;

	if ((ret = dm_fs_init()) || (ret = dm_target_init()))
		return ret;

	/* set up the arrays */
	read_ahead[MAJOR_NR] = DEFAULT_READ_AHEAD;
	blk_size[MAJOR_NR] = _block_size;
	blksize_size[MAJOR_NR] = _blksize_size;
	hardsect_size[MAJOR_NR] = _hardsect_size;

	if (devfs_register_blkdev(MAJOR_NR, _name, &dm_blk_dops) < 0) {
		printk(KERN_ERR "%s -- register_blkdev failed\n", _name);
		return -EIO;
	}

	blk_queue_make_request(BLK_DEFAULT_QUEUE(MAJOR_NR), request);

	_dev_dir = devfs_mk_dir(0, _fs_dir, NULL);


	printk(KERN_INFO "%s %d.%d.%d initialised\n", _name,
	       _version[0], _version[1], _version[2]);
	return 0;
}

static void dm_exit(void)
{
	if(kmem_cache_destroy(_io_hook_cache))
		WARN("it looks like there are still some io_hooks allocated");

	dm_fs_exit();

	if (devfs_unregister_blkdev(MAJOR_NR, _name) < 0)
		printk(KERN_ERR "%s -- unregister_blkdev failed\n", _name);

	read_ahead[MAJOR_NR] = 0;
	blk_size[MAJOR_NR] = 0;
	blksize_size[MAJOR_NR] = 0;
	hardsect_size[MAJOR_NR] = 0;

	printk(KERN_INFO "%s %d.%d.%d cleaned up\n", _name,
	       _version[0], _version[1], _version[2]);
}

/*
 * block device functions
 */
static int blk_open(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	down_write(&_dev_lock);
	md = _devs[minor];

	if (!md || !is_active(md)) {
		up_write(&_dev_lock);
		return -ENXIO;
	}

	md->use_count++;
	up_write(&_dev_lock);

	MOD_INC_USE_COUNT;
	return 0;
}

static int blk_close(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	down_write(&_dev_lock);
	md = _devs[minor];
	if (!md || md->use_count < 1) {
		WARN("reference count in mapped_device incorrect");
		up_write(&_dev_lock);
		return -ENXIO;
	}

	md->use_count--;
	up_write(&_dev_lock);

	MOD_DEC_USE_COUNT;
	return 0;
}

static int blk_ioctl(struct inode *inode, struct file *file,
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

static inline struct io_hook *alloc_io_hook(void)
{
	return kmem_cache_alloc(_io_hook_cache, GFP_NOIO);
}

static inline void free_io_hook(struct io_hook *ih)
{
	kmem_cache_free(_io_hook_cache, ih);
}

/*
 * FIXME: need to decide if deferred_io's need
 * their own slab, I say no for now since they are
 * only used when the device is suspended.
 */
static inline struct deferred_io *alloc_deferred(void)
{
	return kmalloc(sizeof(struct deferred_io), GFP_NOIO);
}

static inline void free_deferred(struct deferred_io *di)
{
	kfree(di);
}

/*
 * bh->b_end_io routine that decrements the
 * pending count and then calls the original
 * bh->b_end_io fn.
 */
static void dec_pending(struct buffer_head *bh, int uptodate)
{
	struct io_hook *ih = bh->b_private;

	if (atomic_dec_and_test(&ih->md->pending))
		/* nudge anyone waiting on suspend queue */
		wake_up_interruptible(&ih->md->wait);

	bh->b_end_io = ih->end_io;
	bh->b_private = ih->context;
	free_io_hook(ih);

	bh->b_end_io(bh, uptodate);
}

/*
 * add the bh to the list of deferred io.
 */
static int queue_io(struct mapped_device *md, struct buffer_head *bh, int rw)
{
	struct deferred_io *di = alloc_deferred();

	if (!di)
		return -ENOMEM;

	down_write(&_dev_lock);
	if (test_bit(DM_ACTIVE, &md->state)) {
		up_write(&_dev_lock);
		return 0;
	}

	di->bh = bh;
	di->rw = rw;
	di->next = md->deferred;
	md->deferred = di;
	up_write(&_dev_lock);

	return 1;
}

/*
 * do the bh mapping for a given leaf
 */
static inline int __map_buffer(struct mapped_device *md,
			       struct buffer_head *bh, int leaf)
{
	dm_map_fn fn;
	void *context;
	struct io_hook *ih = 0;
	int r;
	struct target *ti = md->map->targets + leaf;

	fn = ti->type->map;
	context = ti->private;

	if (!fn)
		return 0;

	ih = alloc_io_hook();

	if (!ih)
		return 0;

	ih->md = md;
	ih->end_io = bh->b_end_io;
	ih->context = bh->b_private;

	r = fn(bh, context);

	if (r > 0) {
		/* hook the end io request fn */
		atomic_inc(&md->pending);
		bh->b_end_io = dec_pending;
		bh->b_private = ih;

	} else if (r == 0)
		/* we don't need to hook */
		free_io_hook(ih);

	else if (r < 0) {
		free_io_hook(ih);
		return 0;
	}

	return 1;
}

/*
 * search the btree for the correct target.
 */
static inline int __find_node(struct dm_table *t, struct buffer_head *bh)
{
	int l, n = 0, k = 0;
	offset_t *node;

	for (l = 0; l < t->depth; l++) {
		n = get_child(n, k);
		node = get_node(t, l, n);

		for (k = 0; k < KEYS_PER_NODE; k++)
			if (node[k] >= bh->b_rsector)
				break;
	}

	return (KEYS_PER_NODE * n) + k;
}

static int request(request_queue_t *q, int rw, struct buffer_head *bh)
{
	struct mapped_device *md;
	int r, minor = MINOR(bh->b_rdev);

	if (minor >= MAX_DEVICES)
		goto bad_no_lock;

	down_read(&_dev_lock);
	md = _devs[minor];

	if (!md || !md->map)
		goto bad;

	/* if we're suspended we have to queue this io for later */
	if (!test_bit(DM_ACTIVE, &md->state)) {
		up_read(&_dev_lock);
		r = queue_io(md, bh, rw);

		if (r < 0)
			goto bad_no_lock;

		else if (r > 0)
			return 0; /* deferred successfully */

		down_read(&_dev_lock);	/* FIXME: there's still a race here */
	}

	if (!__map_buffer(md, bh, __find_node(md->map, bh)))
		goto bad;

	up_read(&_dev_lock);
	return 1;

 bad:
	up_read(&_dev_lock);

 bad_no_lock:
	buffer_IO_error(bh);
	return 0;
}

/*
 * see if the device with a specific minor # is
 * free.
 */
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

/*
 * find the first free device.
 */
static inline int __any_old_dev(void)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++)
		if (!_devs[i])
			return i;

	return -1;
}

/*
 * allocate and initialise a blank device.
 */
static struct mapped_device *alloc_dev(int minor)
{
	struct mapped_device *md = kmalloc(sizeof(*md), GFP_KERNEL);
	memset(md, 0, sizeof(*md));

	down_write(&_dev_lock);
	minor = (minor < 0) ? __any_old_dev() : __specific_dev(minor);

	if (minor < 0) {
		WARN("no free devices available");
		up_write(&_dev_lock);
		kfree(md);
		return 0;
	}

	md->dev = MKDEV(DM_BLK_MAJOR, minor);
	md->name[0] = '\0';
	md->state = 0;

	init_waitqueue_head(&md->wait);

	_devs[minor] = md;
	up_write(&_dev_lock);

	return md;
}

/*
 * open a device so we can use it as a map
 * destination.
 */
static int open_dev(struct dev_list *d)
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

/*
 * close a device that we've been using.
 */
static void close_dev(struct dev_list *d)
{
	blkdev_put(d->bd, BDEV_FILE);
	bdput(d->bd);
	d->bd = 0;
}

/*
 * Open a list of devices.
 */
static int open_devices(struct dev_list *devices)
{
	int r;
	struct dev_list *d, *od;

	/* open all the devices */
	for (d = devices; d; d = d->next)
		if ((r = open_dev(d)))
			goto bad;

	return 0;

 bad:
	od = d;
	for (d = devices; d != od; d = d->next)
		close_dev(d);
	return r;
}

/*
 * Close a list of devices.
 */
static void close_devices(struct dev_list *d)
{
	/* close all the devices */
	while (d) {
		close_dev(d);
		d = d->next;
	}
}

static inline struct mapped_device *__find_by_name(const char *name)
{
	int i;
	for (i = 0; i < MAX_DEVICES; i++)
		if (_devs[i] && !strcmp(_devs[i]->name, name))
			return _devs[i];

	return 0;
}

struct mapped_device *dm_find_by_name(const char *name)
{
	struct mapped_device *md;

	down_read(&_dev_lock);
	md = __find_by_name(name);
	up_read(&_dev_lock);

	return md;
}

struct mapped_device *dm_find_by_minor(int minor)
{
	struct mapped_device *md;

	down_read(&_dev_lock);
	md = _devs[minor];
	up_read(&_dev_lock);

	return md;
}

static int register_device(struct mapped_device *md)
{
	md->devfs_entry =
		devfs_register(_dev_dir, md->name, DEVFS_FL_CURRENT_OWNER,
			       MAJOR(md->dev), MINOR(md->dev),
			       S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP,
			       &dm_blk_dops, NULL);

	if (!md->devfs_entry)
		return -ENOMEM;

	return 0;
}

static int unregister_device(struct mapped_device *md)
{
	devfs_unregister(md->devfs_entry);
	return 0;
}

/*
 * constructor for a new device
 */
int dm_create(const char *name, int minor)
{
	int r;
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	if (!(md = alloc_dev(minor)))
		return -ENOMEM;

	down_write(&_dev_lock);
	if (__find_by_name(name)) {
		WARN("device with that name already exists");
		kfree(md);
		up_write(&_dev_lock);
		return -EINVAL;
	}

	strcpy(md->name, name);
	_devs[minor] = md;

	if ((r = register_device(md))) {
		up_write(&_dev_lock);
		return r;
	}
	up_write(&_dev_lock);

	return 0;
}

/*
 * destructor for the device.  md->map is
 * deliberately not destroyed, dm-fs should manage
 * table objects.
 */
int dm_remove(const char *name)
{
	struct mapped_device *md;
	int minor, r;

	down_write(&_dev_lock);
	if (!(md = __find_by_name(name))) {
		up_write(&_dev_lock);
		return -ENXIO;
	}

	if (md->use_count) {
		up_write(&_dev_lock);
		return -EPERM;
	}

	if ((r = unregister_device(md))) {
		up_write(&_dev_lock);
		return r;
	}

	minor = MINOR(md->dev);
	kfree(md);
	_devs[minor] = 0;
	up_write(&_dev_lock);

	return 0;
}

/*
 * the hardsect size for a mapped device is the
 * smallest hard sect size from the devices it
 * maps onto.
 */
static int __find_hardsect_size(struct dev_list *dl)
{
	int result = INT_MAX, size;

	while(dl) {
		size = get_hardsect_size(dl->dev);
		if (size < result)
			result = size;
		dl = dl->next;
	}

	return result;
}

/*
 * Bind a table to the device.
 */
void __bind(struct mapped_device *md, struct dm_table *t)
{
	int minor = MINOR(md->dev);

	md->map = t;

	_block_size[minor] = (t->highs[t->num_targets - 1] + 1) >> 1;

	/* FIXME: block size depends on the mapping table */
	_blksize_size[minor] = BLOCK_SIZE;
	_hardsect_size[minor] = __find_hardsect_size(t->devices);
	register_disk(NULL, md->dev, 1, &dm_blk_dops, _block_size[minor]);
}

/*
 * requeue the deferred buffer_heads by calling
 * generic_make_request.
 */
static void __flush_deferred_io(struct mapped_device *md)
{
	struct deferred_io *c, *n;

	for (c = md->deferred, md->deferred = 0; c; c = n) {
		n = c->next;
		generic_make_request(c->rw, c->bh);
		free_deferred(c);
	}
}

/*
 * make the device available for use, if was
 * previously suspended rather than newly created
 * then all queued io is flushed
 */
int dm_activate(struct mapped_device *md, struct dm_table *table)
{
	int r;

	/* check that the mapping has at least been loaded. */
	if (!table->num_targets)
		return -EINVAL;

	down_write(&_dev_lock);

	/* you must be deactivated first */
	if (is_active(md)) {
		up_write(&_dev_lock);
		return -EPERM;
	}

	__bind(md, table);

	if ((r = open_devices(md->map->devices))) {
		up_write(&_dev_lock);
		return r;
	}

	set_bit(DM_ACTIVE, &md->state);
	__flush_deferred_io(md);
	up_write(&_dev_lock);

	return 0;
}

/*
 * Deactivate the device, the device must not be
 * opened by anyone.
 */
int dm_deactivate(struct mapped_device *md)
{
	down_read(&_dev_lock);
	if (md->use_count) {
		up_read(&_dev_lock);
		return -EPERM;
	}

	fsync_dev(md->dev);

	up_read(&_dev_lock);

	down_write(&_dev_lock);
	if (md->use_count) {
		/* drat, somebody got in quick ... */
		up_write(&_dev_lock);
		return -EPERM;
	}

        close_devices(md->map->devices);
	md->map = 0;
	clear_bit(DM_ACTIVE, &md->state);
	up_write(&_dev_lock);

	return 0;
}

/*
 * We need to be able to change a mapping table
 * under a mounted filesystem.  for example we
 * might want to move some data in the background.
 * Before the table can be swapped with
 * dm_bind_table, dm_suspend must be called to
 * flush any in flight buffer_heads and ensure
 * that any further io gets deferred.
 */
void dm_suspend(struct mapped_device *md)
{
	DECLARE_WAITQUEUE(wait, current);

	down_write(&_dev_lock);
	if (!is_active(md)) {
		up_write(&_dev_lock);
		return;
	}

	clear_bit(DM_ACTIVE, &md->state);
	up_write(&_dev_lock);

	/* wait for all the pending io to flush */
	add_wait_queue(&md->wait, &wait);
	current->state = TASK_INTERRUPTIBLE;
	do {
		down_write(&_dev_lock);
		if (!atomic_read(&md->pending))
			break;

		up_write(&_dev_lock);
		schedule();

	} while (1);

	current->state = TASK_RUNNING;
	remove_wait_queue(&md->wait, &wait);

	close_devices(md->map->devices);

	md->map = 0;
	up_write(&_dev_lock);
}


struct block_device_operations dm_blk_dops = {
	open:     blk_open,
	release:  blk_close,
	ioctl:    blk_ioctl
};

/*
 * module hooks
 */
module_init(dm_init);
module_exit(dm_exit);

MODULE_DESCRIPTION("device-mapper driver");
MODULE_AUTHOR("Joe Thornber <thornber@btconnect.com>");

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
