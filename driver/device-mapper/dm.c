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
#define DEVICE_NR(device) MINOR(device)	/* has no partition bits */
#define DEVICE_NAME "device-mapper"	/* name for messaging */
#define DEVICE_NO_RANDOM		/* no entropy to contribute */
#define DEVICE_OFF(d)			/* do-nothing */

#include <linux/blk.h>
#include <linux/blkpg.h>

/* we only need this for the lv_bmap struct definition, not happy */
#include <linux/lvm.h>

#define MAX_DEVICES 64
#define DEFAULT_READ_AHEAD 64

const char *_name = "device-mapper";
int _version[3] = {0, 1, 0};

struct io_hook {
	struct mapped_device *md;
	struct target *target;
	int rw;

	void (*end_io)(struct buffer_head * bh, int uptodate);
	void *context;
};

kmem_cache_t *_io_hook_cache;

#define rl down_read(&_dev_lock)
#define ru up_read(&_dev_lock)
#define wl down_write(&_dev_lock)
#define wu up_write(&_dev_lock)

struct rw_semaphore _dev_lock;
static struct mapped_device *_devs[MAX_DEVICES];

/* block device arrays */
static int _block_size[MAX_DEVICES];
static int _blksize_size[MAX_DEVICES];
static int _hardsect_size[MAX_DEVICES];

const char *_fs_dir = "device-mapper";
static devfs_handle_t _dev_dir;

static int request(request_queue_t *q, int rw, struct buffer_head *bh);
static int dm_user_bmap(struct inode *inode, struct lv_bmap *lvb);

/*
 * setup and teardown the driver
 */
static int dm_init(void)
{
	int ret;

	init_rwsem(&_dev_lock);

	if (!_io_hook_cache)
		_io_hook_cache = kmem_cache_create("dm io hooks",
						   sizeof(struct io_hook),
						   0, 0, NULL, NULL);

	if (!_io_hook_cache)
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
	if (kmem_cache_destroy(_io_hook_cache))
		WARN("it looks like there are still some io_hooks allocated");
	_io_hook_cache = 0;

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
static int dm_blk_open(struct inode *inode, struct file *file)
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

static int dm_blk_close(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	wl;
	md = _devs[minor];
	if (!md || md->use_count < 1) {
		WARN("reference count in mapped_device incorrect");
		wu;
		return -ENXIO;
	}

	md->use_count--;
	wu;

	MOD_DEC_USE_COUNT;
	return 0;
}

/* In 512-byte units */
#define VOLUME_SIZE(minor) (_block_size[(minor)] >> 1)

static int dm_blk_ioctl(struct inode *inode, struct file *file,
			uint command, ulong a)
{
	int minor = MINOR(inode->i_rdev);
	long size;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	switch (command) {
	case BLKSSZGET:
	case BLKROGET:
	case BLKROSET:
#if 0
	case BLKELVSET:
	case BLKELVGET:
#endif
		return blk_ioctl(inode->i_dev, command, a);
		break;

	case BLKGETSIZE:
		size = VOLUME_SIZE(minor);
		if (copy_to_user((void *) a, &size, sizeof (long)))
			return -EFAULT;
		break;

	case BLKFLSBUF:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		fsync_dev(inode->i_rdev);
		invalidate_buffers(inode->i_rdev);
		return 0;

	case BLKRAGET:
		if (copy_to_user
		    ((void *) a, &read_ahead[MAJOR(inode->i_rdev)],
		     sizeof (long)))
			return -EFAULT;
		return 0;

	case BLKRASET:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		read_ahead[MAJOR(inode->i_rdev)] = a;
		return 0;

	case BLKRRPART:
		return -EINVAL;

	case LV_BMAP:
		return dm_user_bmap(inode, (struct lv_bmap *) a);

	default:
		WARN("unknown block ioctl %d", command);
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
 * call a targets optional error function if
 * an io failed.
 */
static inline int call_err_fn(struct io_hook *ih, struct buffer_head *bh)
{
	dm_err_fn err = ih->target->type->err;
	if (err)
		return err(bh, ih->rw, ih->target->private);

	return 0;
}

/*
 * bh->b_end_io routine that decrements the
 * pending count and then calls the original
 * bh->b_end_io fn.
 */
static void dec_pending(struct buffer_head *bh, int uptodate)
{
	struct io_hook *ih = bh->b_private;

	if (!uptodate && call_err_fn(ih, bh))
		return;

	if (atomic_dec_and_test(&ih->md->pending))
		/* nudge anyone waiting on suspend queue */
		wake_up(&ih->md->wait);

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

	wl;
	if (test_bit(DM_ACTIVE, &md->state)) {
		wu;
		return 0;
	}

	di->bh = bh;
	di->rw = rw;
	di->next = md->deferred;
	md->deferred = di;
	wu;

	return 1;
}

/*
 * do the bh mapping for a given leaf
 */
static inline int __map_buffer(struct mapped_device *md,
			       struct buffer_head *bh, int rw, int leaf)
{
	int r;
	dm_map_fn fn;
	void *context;
	struct io_hook *ih = NULL;
	struct target *ti = md->map->targets + leaf;

	fn = ti->type->map;
	context = ti->private;

	ih = alloc_io_hook();

	if (!ih)
		return 0;

	ih->md = md;
	ih->rw = rw;
	ih->target = ti;
	ih->end_io = bh->b_end_io;
	ih->context = bh->b_private;

	r = fn(bh, rw, context);

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

	rl;
	md = _devs[minor];

	if (!md || !md->map)
		goto bad;

	/* if we're suspended we have to queue this io for later */
	if (!test_bit(DM_ACTIVE, &md->state)) {
		ru;
		r = queue_io(md, bh, rw);

		if (r < 0)
			goto bad_no_lock;

		else if (r > 0)
			return 0; /* deferred successfully */

		rl;	/* FIXME: there's still a race here */
	}

	if (!__map_buffer(md, bh, rw, __find_node(md->map, bh)))
		goto bad;

	ru;
	return 1;

 bad:
	ru;

 bad_no_lock:
	buffer_IO_error(bh);
	return 0;
}

static int check_dev_size(int minor, unsigned long block)
{
	/* FIXME: check this */
	unsigned long max_sector = (_block_size[minor] << 1) + 1;
	unsigned long sector = (block + 1) * (_blksize_size[minor] >> 9);

	return (sector > max_sector) ? 0 : 1;
}

/*
 * creates a dummy buffer head and maps it (for lilo).
 */
static int do_bmap(kdev_t dev, unsigned long block,
		   kdev_t *r_dev, unsigned long *r_block)
{
	struct mapped_device *md;
	struct buffer_head bh;
	int minor = MINOR(dev), r;
	struct target *t;

	rl;
	if ((minor >= MAX_DEVICES) || !(md = _devs[minor]) ||
	    !test_bit(DM_ACTIVE, &md->state)) {
		r = -ENXIO;
		goto out;
	}

	if (!check_dev_size(minor, block)) {
		r = -EINVAL;
		goto out;
	}

	/* setup dummy bh */
	memset(&bh, 0, sizeof(bh));
	bh.b_blocknr = block;
	bh.b_dev = bh.b_rdev = dev;
	bh.b_size = _blksize_size[minor];
	bh.b_rsector = block * (bh.b_size >> 9);

	/* find target */
	t = md->map->targets + __find_node(md->map, &bh);

	/* do the mapping */
	r = t->type->map(&bh, READ, t->private);

	*r_dev = bh.b_rdev;
	*r_block = bh.b_rsector / (bh.b_size >> 9);

 out:
	ru;
	return r;
}

/*
 * marshals arguments and results between user and
 * kernel space.
 */
static int dm_user_bmap(struct inode *inode, struct lv_bmap *lvb)
{
	unsigned long block, r_block;
	kdev_t r_dev;
	int r;

	if (get_user(block, &lvb->lv_block))
		return -EFAULT;

	if ((r = do_bmap(inode->i_rdev, block, &r_dev, &r_block)))
		return r;

	if (put_user(kdev_t_to_nr(r_dev), &lvb->lv_dev) ||
	    put_user(r_block, &lvb->lv_block))
		return -EFAULT;

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

	if (!md)
		return 0;

	memset(md, 0, sizeof (*md));

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

	init_waitqueue_head(&md->wait);

	_devs[minor] = md;
	wu;

	return md;
}

/*
 * open a device so we can use it as a map
 * destination.
 */
static int open_dev(struct dm_dev *d)
{
       int err;

       if (d->bd)
	       BUG();

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
static void close_dev(struct dm_dev *d)
{
	if (!d->bd)
		return;

	blkdev_put(d->bd, BDEV_FILE);
	bdput(d->bd);
	d->bd = 0;
}

/*
 * Close a list of devices.
 */
static void close_devices(struct list_head *devices)
{
	struct list_head *tmp;

	for (tmp = devices->next; tmp != devices; tmp = tmp->next) {
		struct dm_dev *dd = list_entry(tmp, struct dm_dev, list);
		close_dev(dd);
	}
}

/*
 * Open a list of devices.
 */
static int open_devices(struct list_head *devices)
{
	int r = 0;
	struct list_head *tmp;

	for (tmp = devices->next; tmp != devices; tmp = tmp->next) {
		struct dm_dev *dd = list_entry(tmp, struct dm_dev, list);
		if ((r = open_dev(dd)))
			goto bad;
	}
	return 0;

 bad:
	close_devices(devices);
	return r;
}


struct mapped_device *dm_find_by_minor(int minor)
{
	struct mapped_device *md;

	rl;
	md = _devs[minor];
	ru;

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
struct mapped_device *dm_create(const char *name, int minor)
{
	int r;
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return ERR_PTR(-ENXIO);

	if (!(md = alloc_dev(minor)))
		return ERR_PTR(-ENXIO);

	wl;
	strcpy(md->name, name);
	_devs[minor] = md;
	if ((r = register_device(md))) {
		wu;
		return ERR_PTR(r);
	}
	wu;

	return md;
}

/*
 * destructor for the device.  md->map is
 * deliberately not destroyed, dm-fs should manage
 * table objects.
 */
int dm_remove(struct mapped_device *md)
{
	int minor, r;

	wl;
	if (md->use_count) {
		wu;
		return -EPERM;
	}

	if ((r = unregister_device(md))) {
		wu;
		return r;
	}

	minor = MINOR(md->dev);
	_devs[minor] = 0;
	wu;

	kfree(md);

	return 0;
}

/*
 * the hardsect size for a mapped device is the
 * smallest hard sect size from the devices it
 * maps onto.
 */
static int __find_hardsect_size(struct list_head *devices)
{
	int result = INT_MAX, size;
	struct list_head *tmp;

	for (tmp = devices->next; tmp != devices; tmp = tmp->next) {
		struct dm_dev *dd = list_entry(tmp, struct dm_dev, list);
		size = get_hardsect_size(dd->dev);
		if (size < result)
			result = size;
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

	/* in k */
	_block_size[minor] = (t->highs[t->num_targets - 1] + 1) >> 1;

	_blksize_size[minor] = BLOCK_SIZE;
	_hardsect_size[minor] = __find_hardsect_size(&t->devices);
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

	wl;

	/* you must be deactivated first */
	if (is_active(md)) {
		wu;
		return -EPERM;
	}

	__bind(md, table);

	if ((r = open_devices(&md->map->devices))) {
		wu;
		return r;
	}

	set_bit(DM_ACTIVE, &md->state);
	__flush_deferred_io(md);
	wu;

	return 0;
}

/*
 * Deactivate the device, the device must not be
 * opened by anyone.
 */
int dm_deactivate(struct mapped_device *md)
{
	rl;
	if (md->use_count) {
		ru;
		return -EPERM;
	}

	fsync_dev(md->dev);

	ru;

	wl;
	if (md->use_count) {
		/* drat, somebody got in quick ... */
		wu;
		return -EPERM;
	}

	close_devices(&md->map->devices);
	md->map = 0;
	clear_bit(DM_ACTIVE, &md->state);
	wu;

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

	wl;
	if (!is_active(md)) {
		wu;
		return;
	}

	clear_bit(DM_ACTIVE, &md->state);
	wu;

	/* wait for all the pending io to flush */
	add_wait_queue(&md->wait, &wait);
	current->state = TASK_UNINTERRUPTIBLE;
	do {
		wl;
		if (!atomic_read(&md->pending))
			break;

		wu;
		schedule();

	} while (1);

	current->state = TASK_RUNNING;
	remove_wait_queue(&md->wait, &wait);
	close_devices(&md->map->devices);

	md->map = 0;
	wu;
}

struct block_device_operations dm_blk_dops = {
	open:	  dm_blk_open,
	release:  dm_blk_close,
	ioctl:	  dm_blk_ioctl
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
