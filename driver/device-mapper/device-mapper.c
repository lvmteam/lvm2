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

/*
 * defines for blk.h
 */
#define MAJOR_NR DM_BLK_MAJOR
#define DEVICE_OFF(device)
#define LOCAL_END_REQUEST
#define MAX_DEPTH 16
#define NODE_SIZE L1_CACHE_BYTES
#define KEYS_PER_NODE (NODE_SIZE / sizeof(offset_t))


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
 * 'register_mapping_type' function should be called.  A target type
 * is specified using three functions (see the header):
 *
 * dm_ctr_fn - takes a string and contructs a target specific piece of
 *             context data.
 * dm_dtr_fn - destroy contexts.
 * dm_map_fn - function that takes a buffer_head and some previously
 *             constructed context and performs the remapping.
 *
 * This file contains two trivial mappers, which are automatically registered:
 * 'linear', and 'io_error'. Linear alone would be enough to implement most
 * LVM features (omitting striped volumes and snapshots).
 *
 * At the moment this driver has a temporary ioctl interface, but I will
 * move this to a read/write interface on either a /proc file or a
 * char device.  This will allow scripts to simply cat a text mapping
 * table in order to set up a volume.
 *
 * At the moment the table assumes 32 bit keys (sectors), the move to
 * 64 bits will involve no interface changes, since the tables wil be
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
 * the copying process had completed.
 */

#define MAX_DEVICES 64
#define DEFAULT_READ_AHEAD 64

#define WARN(f, x...) printk(KERN_WARNING "%s " f, _name , ## x)

const char *_name = "device-mapper";
int _version[3] = {1, 0, 0};


struct mapper {
	char *name;
	dm_ctr_fn ctr;
	dm_dtr_fn dtr;
	dm_map_fn map;

	struct mapper *next;
};

struct mapped_device {
	int in_use;
	spinlock_t lock;
	kdev_t dev;

	atomic_t pending;
	int depth;
	int counts[MAX_DEPTH];	/* in nodes */
	offset_t *index[MAX_DEPTH];

	int num_targets;
	dm_map_fn *targets;
	void **contexts;
};


static int _dev_count = 0;
static struct mapped_device _devices[MAX_DEVICES];

static spinlock_t _mappers_lock = SPIN_LOCK_UNLOCKED;
static struct mapper *_mappers;

/* block device arrays */
static int _block_size[MAX_DEVICES];
static int _blksize_size[MAX_DEVICES];
static int _hardsect_size[MAX_DEVICES];

static int _ctl_open(struct inode *inode, struct file *file);
static int _ctl_close(struct inode *inode, struct file *file);
static int _ctl_ioctl(struct inode *inode, struct file *file,
		      uint command, ulong a);

static struct file_operations _ctl_fops = {
	open:     _ctl_open,
	release:  _ctl_close,
	ioctl:    _ctl_ioctl,
};

static int _blk_open(struct inode *inode, struct file *file);
static int _blk_close(struct inode *inode, struct file *file);
static int _blk_ioctl(struct inode *inode, struct file *file,
		      uint command, ulong a);

static struct block_device_operations _blk_dops = {
	open:     _blk_open,
	release:  _blk_close,
	ioctl:    _blk_ioctl
};

static void _init_mds(void);
static struct mapped_device *_build_map(struct device_table *t);
static int _request_fn(request_queue_t *q, int rw, struct buffer_head *bh);
static int _register_std_targets(void);
static struct mapped_device *_get_free_md(void);
static void _put_free_md(struct mapped_device *md);
static int _setup_targets(struct mapped_device *md, struct device_table *t);
static int _setup_btree(struct mapped_device *md, struct device_table *t);
static int _setup_btree_index(int l, struct mapped_device *md);
struct mapper *_find_mapper(const char *name);

/*
 * setup and teardown the driver
 */
static int _init(void)
{
	_init_mds();

	if (!_register_std_targets())
		return -EIO;	/* FIXME: better error value */

	/* set up the arrays */
	read_ahead[MAJOR_NR] = DEFAULT_READ_AHEAD;
	blk_size[MAJOR_NR] = _block_size;
	blksize_size[MAJOR_NR] = _blksize_size;
	hardsect_size[MAJOR_NR] = _hardsect_size;

	if (register_chrdev(DM_CTL_MAJOR, _name, &_ctl_fops) < 0) {
		printk(KERN_ERR "%s - register_chrdev failed\n", _name);
		return -EIO;
	}

	if (register_blkdev(MAJOR_NR, _name, &_blk_dops) < 0) {
		printk(KERN_ERR "%s -- register_blkdev failed\n", _name);
		if (unregister_chrdev(DM_CTL_MAJOR, _name) < 0)
			printk(KERN_ERR "%s - unregister_chrdev failed\n",
			       _name);
		return -EIO;
	}

	blk_queue_make_request(BLK_DEFAULT_QUEUE(MAJOR_NR), _request_fn);

	printk(KERN_INFO "%s(%d, %d, %d) successfully initialised\n", _name,
	       _version[0], _version[1], _version[2]);
	return 0;
}

static void _fin(void)
{
	if (unregister_chrdev(DM_CTL_MAJOR, _name) < 0)
		printk(KERN_ERR "%s - unregister_chrdev failed\n", _name);

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
 * character device fns
 */
static int _ctl_open(struct inode *inode, struct file *file)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	MOD_INC_USE_COUNT;
	return 0;
}

static int _ctl_close(struct inode *inode, struct file *file)
{
	MOD_DEC_USE_COUNT;
	return 0;
}

static int _ctl_ioctl(struct inode *inode, struct file *file,
		      uint command, ulong a)
{
	struct device_table info;

	if (copy_from_user(&info, (void *) a, sizeof(info)))
	   return -EFAULT;

	switch (command) {
	case MAPPED_DEVICE_CREATE:
		/* FIXME: copy arrays */
		_build_map(&info);
		break;

	case MAPPED_DEVICE_DESTROY:
		/* FIXME: finish */
		break;


	default:
		printk(KERN_WARNING "%s -- _ctl_ioctl: unknown command 0x%x\n",
		       _name, command);
		return -EINVAL;
	}

	return 0;
}


/*
 * block device functions
 */
static int _blk_open(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md = _devices + minor;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	spin_lock(&md->lock);

	if (!md->in_use) {
		spin_unlock(&md->lock);
		return -ENXIO;
	}

	md->in_use++;
	spin_unlock(&md->lock);

	MOD_INC_USE_COUNT;
	return 0;
}

static int _blk_close(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md = _devices + minor;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	spin_lock(&md->lock);

	if (md->in_use <= 1) {
		WARN("reference count in mapped_device incorrect\n");
		spin_unlock(&md->lock);
		return -ENXIO;
	}

	md->in_use--;
	spin_unlock(&md->lock);

	MOD_INC_USE_COUNT;
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
		printk(KERN_WARNING "%s - unknown block ioctl %d\n",
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
	int minor = MINOR(bh->b_dev);
	dm_map_fn fn;
	void *context;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	md = _devices + minor;
	if (MINOR(md->dev != minor))
		return -ENXIO;

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

	return ret;
}


static struct mapped_device *_build_map(struct device_table *t)
{
	struct mapped_device *md = _get_free_md();

	if (!md)
		return 0;

	if (!_setup_targets(md, t))
		goto bad;

	if (!_setup_btree(md, t))
		goto bad;

	return md;

 bad:
	_put_free_md(md);
	return 0;
}

static inline void *__aligned(size_t s, unsigned int align)
{
	return vmalloc(s);
}

static inline void __free_aligned(void *ptr)
{
	vfree(ptr);
}

static int _setup_targets(struct mapped_device *md, struct device_table *t)
{
	int i;
	offset_t low = 0;

	md->num_targets = t->count;
	md->targets = __aligned(sizeof(*md->targets) * md->num_targets,
			       NODE_SIZE);

	for (i = 0; i < md->num_targets; i++) {
		struct mapper *m = _find_mapper(t->map[i].type);
		if (!m)
			return 0;

		if (!m->ctr(low, t->map[i].high + 1,
			    t->map[i].context, md->contexts + i)) {
			WARN("contructor for '%s' failed\n", m->name);
			return 0;
		}

		md->targets[i] = m->map;
	}

	return 1;
}

static inline ulong _round_up(ulong n, ulong size)
{
	ulong r = n % size;
	return ((n / size) + (r ? 1 : 0)) * size;
}

static inline ulong _div_up(ulong n, ulong size)
{
	return _round_up(n, size) / size;
}

static int _setup_btree(struct mapped_device *md, struct device_table *t)
{
	int n, i;
	offset_t *k;

	/* how many indexes will the btree have ? */
	for (n = _div_up(md->num_targets, KEYS_PER_NODE), i = 1; n != 1; i++)
		n = _div_up(n, KEYS_PER_NODE + 1);

	md->depth = i;
	md->counts[md->depth - 1] = _div_up(md->num_targets, KEYS_PER_NODE);

	while (--i)
		md->counts[i - 1] = _div_up(md->counts[i], KEYS_PER_NODE + 1);

	for (i = 0; i < md->depth; i++) {
		size_t s = NODE_SIZE * md->counts[i];
		md->index[i] = __aligned(s, NODE_SIZE);
		memset(md->index[i], -1, s);
	}

	/* bottom layer is easy */
	for (k = md->index[md->depth - 1], i = 0; i < md->num_targets; i++)
		k[i] = t->map[i].high;

	/* fill in higher levels */
	for (i = md->depth - 1; i; i--)
		_setup_btree_index(i - 1, md);

	return 1;
}

static offset_t __high(struct mapped_device *md, int l, int n)
{
	while (1) {
		if (n >= md->counts[l])
			return (offset_t) -1;

		if (l == md->depth - 1)
			return md->index[l][((n + 1) * KEYS_PER_NODE) - 1];

		l++;
		n = (n + 1) * (KEYS_PER_NODE + 1) - 1;
	}
}

static int _setup_btree_index(int l, struct mapped_device *md)
{
	int n, c, cn;

	for (n = 0, cn = 0; n < md->counts[l]; n++) {
		offset_t *k = md->index[l] + (n * KEYS_PER_NODE);

		for (c = 0; c < KEYS_PER_NODE; c++)
			k[c] = __high(md, l + 1, cn++);
		cn++;
	}

	return 1;
}

static void _init_mds(void)
{
	int i;

	_dev_count = 0;
	memset(_devices, 0, sizeof(_devices));
	for (i = 0; i < MAX_DEVICES; i++) {
		_devices[i].lock = SPIN_LOCK_UNLOCKED;
		_devices[i].dev = MKDEV(MAJOR_NR, i);
	}
}

static struct mapped_device *_get_free_md(void)
{
	int i;
	struct mapped_device *m;

	for (i = 0; i < MAX_DEVICES; i++) {
		m = _devices + i;

		spin_lock(&m->lock);
		if (!m->in_use) {
			m->in_use = 1;
			spin_unlock(m);
			return m;
		}
		spin_unlock(&m->lock);
	}

	WARN("no free devices available\n");
	return 0;
}

static void _put_free_md(struct mapped_device *md)
{
	int i;

	spin_lock(&md->lock);

	for (i = 0; i < md->depth; i++)
		__free_aligned(md->index[i]);

	__free_aligned(md->targets);
	__free_aligned(md->contexts);

	/* FIXME: check this is the correct length */
	memset(&md->depth, 0,
	       sizeof(*md) - ((void *) &md->depth - (void *) md));
	md->in_use = 0;
	spin_unlock(&md->lock);
}

int register_mapping_type(const char *name, dm_ctr_fn ctr,
			  dm_dtr_fn dtr, dm_map_fn map)
{
	struct mapper *m;

	if (_find_mapper(name)) {
		WARN("mapper(%s) already registered\n", name);
		return -1;	/* FIXME: what's a good return value ? */
	}

	/* FIXME: There's a race between the last check and insertion */

	if ((m = kmalloc(sizeof(*m) + strlen(name) + 1, GFP_KERNEL))) {
		WARN("couldn't allocate memory for mapper\n");
		return -ENOMEM;
	}

	m->name = (char *) (m + 1);
	m->ctr = ctr;
	m->dtr = dtr;
	m->map = map;

	spin_lock(&_mappers_lock);
	m->next = _mappers;
	_mappers = m;
	spin_unlock(&_mappers_lock);

	return 0;
}

struct mapper *_find_mapper(const char *name)
{
	struct mapper *m;

	spin_lock(&_mappers_lock);
	for (m = _mappers; m && strcmp(m->name, name); m = m->next)
		;
	spin_unlock(&_mappers_lock);

	return m;
}


/*
 * now for a couple of simple targets:
 *
 * 'io-err' target always fails an io, useful for bringing up LV's
 * that have holes in them.
 *
 * 'linear' target maps a linear range of a device
 */
int _io_err_ctr(offset_t b, offset_t e, const char *context, void **result)
{
	/* this takes no arguments */
	*result = 0;
	return 1;
}

void _io_err_dtr(void *c)
{
	/* empty */
}

int _io_err_map(struct buffer_head *bh, void *context)
{
	buffer_IO_error(bh);
	return 0;
}


struct linear_c {
	kdev_t dev;
	int offset;		/* FIXME: we need a signed offset type */
};

int _linear_ctr(offset_t b, offset_t e, const char *context, void **result)
{
	/* context string should be of the form:
	 *  <major> <minor> <offset>
	 */
	char *ptr = (char *) context;
	struct linear_c *lc;
	int major, minor, start;

	/* FIXME: somewhat verbose */
	major = simple_strtol(context, &ptr, 10);
	if (ptr == context)
		return 0;

	context = ptr;
	minor = simple_strtol(context, &ptr, 10);
	if (ptr == context)
		return 0;

	context = ptr;
	start = simple_strtoul(context, &ptr, 10);
	if (ptr == context)
		return 0;

	if (!(lc = kmalloc(sizeof(lc), GFP_KERNEL))) {
		WARN("couldn't allocate memory for linear context\n");
		return 0;
	}

	lc->dev = MKDEV(major, minor);
	lc->offset = start - b;

	/* FIXME: we should open the PV */

	*result = lc;
	return 1;
}

void _linear_dtr(void *c)
{
	kfree(c);
}

int _linear_map(struct buffer_head *bh, void *context)
{
	struct linear_c *lc = (struct linear_c *) context;

	bh->b_rdev = lc->dev;
	bh->b_rsector = bh->b_rsector + lc->offset;
	return 1;
}

static int _register_std_targets(void)
{
	int ret;

#define xx(n, fn) \
	if ((ret = register_mapping_type(n, \
             fn ## _ctr, fn ## _dtr, fn ## _map) < 0)) return ret

	xx("io-err", _io_err);
	xx("linear", _linear);
#undef xx

	return 0;
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
