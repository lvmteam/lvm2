/*
 * dm-blkdev.c
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
#include <linux/slab.h>
#include <linux/list.h>
#include "dm.h"

struct dm_bdev {
	struct list_head list;
	struct block_device *bdev;
	int use;
};

/* 
 * Lock ordering: Always get bdev_sem before bdev_lock if you need both locks.
 *
 * bdev_lock: A spinlock which protects the list
 * bdev_sem: A semaphore which protects blkdev_get / blkdev_put so that we
 *           are certain to hold only a single reference at any point in time.
 */
static kmem_cache_t *bdev_cachep;
static LIST_HEAD(bdev_list);
static rwlock_t bdev_lock = RW_LOCK_UNLOCKED;
static DECLARE_MUTEX(bdev_sem);

static struct dm_bdev *__dm_find_device(struct block_device *bdev)
{
	struct list_head *tmp, *head;
	struct dm_bdev *b;

	tmp = head = &bdev_list;
	for(;;) {
		tmp = tmp->next;
		if (tmp == head)
			break;
		b = list_entry(tmp, struct dm_bdev, list);
		if (b->bdev != bdev)
			continue;
		b->use++;
		return b;
	}

	return NULL;
}

static struct dm_bdev *dm_get_device(struct block_device *bdev)
{
	struct dm_bdev *d, *n;
	int rv = 0;

	read_lock(&bdev_lock);
	d = __dm_find_device(bdev);
	read_unlock(&bdev_lock);

	if (d)
		return d;

	n = kmem_cache_alloc(bdev_cachep, GFP_KERNEL);
	if (!n)
		return ERR_PTR(-ENOMEM);

	n->bdev = bdev;
	n->use = 1;

	down(&bdev_sem);
	read_lock(&bdev_lock);
	d = __dm_find_device(bdev);
	read_unlock(&bdev_lock);

	
	if (!d) {
		rv = blkdev_get(d->bdev, FMODE_READ | FMODE_WRITE, 0, BDEV_FILE);
		if (rv == 0) {
			write_lock(&bdev_lock);
			list_add(&bdev_list, &n->list);
			d = n;
			n = NULL;
			write_unlock(&bdev_lock);
		}
	}
	if (n) {
		kmem_cache_free(bdev_cachep, n);
	}
	if (rv) {
		d = ERR_PTR(rv);
	}
	up(&bdev_sem);

	return d;
}

struct dm_bdev *dm_blkdev_get(const char *path)
{
	struct dm_bdev *d;
	struct nameidata nd;
	struct inode *inode;

	if (!path_init(path, LOOKUP_FOLLOW, &nd))
		return ERR_PTR(-EINVAL);

	if (path_walk(path, &nd))
		return ERR_PTR(-EINVAL);

	inode = nd.dentry->d_inode;
	if (!inode) {
		d = ERR_PTR(-ENOENT);
		goto out;
	}

	d = dm_get_device(inode->i_bdev);

out:
	path_release(&nd);
	return d;
}

static void dm_blkdev_drop(struct dm_bdev *d)
{
	down(&bdev_sem);
	write_lock(&bdev_lock);
	if (d->use == 0) {
		list_del(&d->list);
	} else {
		d = NULL;
	}
	write_unlock(&bdev_lock);
	if (d) {
		blkdev_put(d->bdev, BDEV_FILE);
		kmem_cache_free(bdev_cachep, d);
	}
	up(&bdev_sem);
}

void dm_blkdev_put(struct dm_bdev *d)
{
	int do_drop = 0;

	read_lock(&bdev_lock);
	if (--d->use == 0)
		do_drop = 1;
	read_unlock(&bdev_lock);

	if (do_drop)
		dm_blkdev_drop(d);
}

EXPORT_SYMBOL(dm_blkdev_get);
EXPORT_SYMBOL(dm_blkdev_put);

