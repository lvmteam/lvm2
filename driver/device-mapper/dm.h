/*
 * dm.h
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
 * Internal header file for device mapper
 *
 * Changelog
 *
 *     16/08/2001 - First version [Joe Thornber]
 */

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
 * (dm-table.c) A btree like structure is used to hold the sector
 * range -> target mapping.  Because we know all the entries in the
 * btree in advance we can make a very compact tree, omitting pointers
 * to child nodes, (child nodes locations can be calculated). Each
 * node of the btree is 1 level cache line in size, this gives a small
 * performance boost.
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
 * Of course these results should be taken with a pinch of salt; the
 * lookups were sequential and there were no other applications (other
 * than X + emacs) running to give any pressure on the level 1 cache.
 *
 * Typical LVM users would find they have very few targets for each
 * LV (probably less than 10).
 *
 * (dm-target.c) Target types are not hard coded, instead the
 * register_mapping_type function should be called.  A target type is
 * specified using three functions (see the header):
 *
 * dm_ctr_fn - takes a string and contructs a target specific piece of
 *             context data.
 * dm_dtr_fn - destroy contexts.
 * dm_map_fn - function that takes a buffer_head and some previously
 *             constructed context and performs the remapping.
 *
 * Currently there are two two trivial mappers, which are
 * automatically registered: 'linear', and 'io_error'.  Linear alone
 * is enough to implement most LVM features (omitting striped volumes
 * and snapshots).
 *
 * (dm-fs.c) The driver is controlled through a /proc interface:
 * /proc/device-mapper/control allows you to create and remove devices
 * by 'cat'ing a line of the following format:
 *
 * create <device name> [minor no]
 * remove <device name>
 *
 * /proc/device-mapper/<device name> accepts the mapping table:
 *
 * begin
 * <sector start> <length> <target name> <target args>...
 * ...
 * end
 *
 * The begin/end lines are nasty, they should be handled by open/close
 * for the file.
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
 * nasty 64 bit %'s.
 *
 * mirror mapping (reflector ?); would set off a kernel thread slowly
 * copying data from one region to another, ensuring that any new
 * writes got copied to both destinations correctly.  Great for
 * implementing pvmove.  Not sure how userland would be notified that
 * the copying process had completed.  Possibly by reading a /proc entry
 * for the LV.  Could also use poll() for this kind of thing.
 */


#ifndef DM_INTERNAL_H
#define DM_INTERNAL_H

#include <linux/config.h>
#include <linux/version.h>
#include <linux/major.h>
#include <linux/iobuf.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/compatmac.h>
#include <linux/cache.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/ctype.h>
#include <linux/device-mapper.h>
#include <linux/list.h>

#define MAX_DEPTH 16
#define NODE_SIZE L1_CACHE_BYTES
#define KEYS_PER_NODE (NODE_SIZE / sizeof(offset_t))
#define CHILDREN_PER_NODE (KEYS_PER_NODE + 1)
#define DM_NAME_LEN 128
#define MAX_TARGET_LINE 256

enum {
	DM_BOUND = 0,		/* device has been bound to a table */
	DM_ACTIVE,		/* device is running */
};

/*
 * io that had to be deferred while we were
 * suspended
 */
struct deferred_io {
	int rw;
	struct buffer_head *bh;
	struct deferred_io *next;
};

/*
 * btree leaf, these do the actual mapping
 */
struct target {
	struct target_type *type;
	void *private;
};

/*
 * the btree
 */
struct dm_table {
	atomic_t refcnt;

	/* btree table */
	int depth;
	int counts[MAX_DEPTH];	/* in nodes */
	offset_t *index[MAX_DEPTH];

	int blksize_size;
	int hardsect_size;
	int num_targets;
	int num_allocated;
	offset_t *highs;
	struct target *targets;

	atomic_t pending;
	wait_queue_head_t wait;
};

/*
 * the actual device struct
 */
struct mapped_device {
	kdev_t dev;
	char name[DM_NAME_LEN];
	struct inode *inode;

	int use_count;
	int state;

	/* a list of io's that arrived while we were suspended */
	struct deferred_io *deferred;

	struct dm_table *map;

	/* used by dm-fs.c */
	devfs_handle_t devfs_entry;
	struct proc_dir_entry *pde;
};

extern struct block_device_operations dm_blk_dops;


/* dm-target.c */
struct target_type *dm_get_target_type(const char *name);
void dm_put_target_type(struct target_type *t);
int dm_target_init(void);

/* dm.c */
struct mapped_device *dm_find_by_name(const char *name);
struct mapped_device *dm_find_by_minor(int minor);

struct mapped_device *dm_create(const char *name, int minor);
int dm_remove(struct mapped_device *md);

int dm_activate(struct mapped_device *md, struct dm_table *t);
int dm_deactivate(struct mapped_device *md);

void dm_suspend(struct mapped_device *md);


/* dm-table.c */
struct dm_table *dm_table_create(void);
void dm_put_table(struct dm_table *t);

int dm_table_add_target(struct dm_table *t, offset_t high,
			struct target_type *type, void *private);
int dm_table_complete(struct dm_table *t);

/* dm-parse.c */
typedef int (*extract_line_fn)(struct text_region *line,
			       void *private);

struct dm_table *dm_parse(extract_line_fn line_fn, void *line_private,
			  dm_error_fn err_fn, void *err_private);


static inline int dm_empty_tok(struct text_region *txt)
{
	return txt->b >= txt->e;
}

/* dm-blkdev.c */
int dm_init_blkdev(void);
void dm_cleanup_blkdev(void);

/* dm-fs.c */
int dmfs_init(void);
void dmfs_exit(void);



#define WARN(f, x...) printk(KERN_WARNING "device-mapper: " f "\n" , ## x)

/*
 * calculate the index of the child node of the
 * n'th node k'th key.
 */
static inline int get_child(int n, int k)
{
	return (n * CHILDREN_PER_NODE) + k;
}

/*
 * returns the n'th node of level l from table t.
 */
static inline offset_t *get_node(struct dm_table *t, int l, int n)
{
	return t->index[l] + (n * KEYS_PER_NODE);
}

static inline int is_active(struct mapped_device *md)
{
	return test_bit(DM_ACTIVE, &md->state);
}

/*
 * FIXME: these are too big to be inlines
 */

#endif
