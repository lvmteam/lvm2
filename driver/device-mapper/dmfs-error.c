/*
 * dmfs-error.c
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
#include <linux/list.h>
#include <linux/fs.h>

#include "dm.h"

struct dmfs_error {
	struct list_head list;
	unsigned len;
	char *msg;
};

void dmfs_add_error(struct dm_table *t, unsigned num, char *str)
{
	int len = strlen(str) + sizeof(struct dmfs_error) + 12;
	struct dmfs_error *e = kmalloc(len, GFP_KERNEL);
	if (e) {
		e->msg = (char *)(e + 1);
		e->len = sprintf(e->msg, "%8u: %s\n", num, str);
		list_add(&e->list, &t->errors);
	}
}

void dmfs_zap_errors(struct dm_table *t)
{
	struct dmfs_error *e;

	while(!list_empty(&t->errors)) {
		e = list_entry(t->errors.next, struct dmfs_error, list);
		kfree(e);
	}
}

static struct dmfs_error *find_initial_message(struct dm_table *t, loff_t *pos)
{
	struct dmfs_error *e;
	struct list_head *tmp, *head;

	tmp = head = &t->errors;
	for(;;) {
		tmp = tmp->next;
		if (tmp == head)
			break;
		e = list_entry(tmp, struct dmfs_error, list);
		if (*pos < e->len)
			return e;
		(*pos) -= e->len;
	}

	return NULL;
}

static int copy_sequence(struct dm_table *t, struct dmfs_error *e, char *buf,
			size_t size, loff_t offset)
{
	char *from;
	int amount;
	int copied = 0;

	do {
		from = e->msg + offset;
		amount = e->len - offset;

		if (copy_to_user(buf, from, amount))
			return -EFAULT;

		buf += amount;
		copied += amount;
		size -= amount;
		offset = 0;

		if (e->list.next == &t->errors)
			break;
		e = list_entry(e->list.next, struct dmfs_error, list);
	} while(size > 0);

	return amount;
}

static ssize_t dmfs_error_read(struct file *file, char *buf, size_t size, loff_t *pos)
{
	struct dmfs_i *dmi = DMFS_I(file->f_dentry->d_parent->d_inode);
	struct dm_table *t = dmi->table;
	int copied = 0;
	loff_t offset = *pos;

	if (!access_ok(VERIFY_WRITE, buf, size))
		return -EFAULT;

	down(&dmi->sem);
	if (dmi->table) {
		struct dmfs_error *e = find_initial_message(t, &offset);
		if (e) {
			copied = copy_sequence(t, e, buf, size, offset);
			if (copied > 0)
				(*pos) += copied;
		}
	}
	up(&dmi->sem);
	return copied;
}

static int dmfs_error_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct file_operations dmfs_error_file_operations = {
	read:		dmfs_error_read,
	fsync:		dmfs_error_sync,
};

static struct inode_operations dmfs_error_inode_operations = {
};

struct inode *dmfs_create_error(struct inode *dir, int mode)
{
	struct inode *inode = new_inode(dir->i_sb);

	if (inode) {
		inode->i_mode = mode | S_IFREG;
		inode->i_uid = current->fsuid;
		inode->i_gid = current->fsgid;
		inode->i_blksize = PAGE_CACHE_SIZE;
		inode->i_blocks = 0;
		inode->i_rdev = NODEV;
		inode->i_atime = inode->i_ctime = inode->i_mtime = CURRENT_TIME;
		inode->i_fop = &dmfs_error_file_operations;
		inode->i_op = &dmfs_error_inode_operations;
	}

	return inode;
}

