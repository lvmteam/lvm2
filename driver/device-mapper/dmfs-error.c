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
#include <linux/seq_file.h>

#include "dm.h"
#include "dmfs.h"

struct dmfs_error {
	struct list_head list;
	unsigned len;
	char *msg;
};

static struct dmfs_error oom_error;

static struct list_head oom_list = {
	next: &oom_error.list,
	prev: &oom_error.list,
};

static struct dmfs_error oom_error = {
	list: { next: &oom_list, prev: &oom_list },
	len: 39,
	msg: "Out of memory during creation of table\n",
};

void dmfs_add_error(struct inode *inode, unsigned num, char *str)
{
	struct dmfs_i *dmi = DMFS_I(inode);
	int len = strlen(str) + sizeof(struct dmfs_error) + 12;
	struct dmfs_error *e = kmalloc(len, GFP_KERNEL);
	if (e) {
		e->msg = (char *)(e + 1);
		e->len = sprintf(e->msg, "%8u: %s\n", num, str);
		list_add(&e->list, &dmi->errors);
	}
}

void dmfs_zap_errors(struct inode *inode)
{
	struct dmfs_i *dmi = DMFS_I(inode);
	struct dmfs_error *e;

	while(!list_empty(&dmi->errors)) {
		e = list_entry(dmi->errors.next, struct dmfs_error, list);
		list_del(&e->list);
		kfree(e);
	}
}

static void *e_start(struct seq_file *e, loff_t *pos)
{
	struct list_head *p;
	loff_t n = *pos;
	struct dmfs_i *dmi = e->context;

	down(&dmi->sem);
	if (dmi->status) {
		list_for_each(p, &oom_list)
			if (n-- == 0)
				return list_entry(p, struct dmfs_error, list);
	} else {
		list_for_each(p, &dmi->errors)
			if (n-- == 0)
				return list_entry(p, struct dmfs_error, list);
	}

	return NULL;
}

static void *e_next(struct seq_file *e, void *v, loff_t *pos)
{
	struct dmfs_i *dmi = e->context;
	struct list_head *p = ((struct dmfs_error *)v)->list.next;
	(*pos)++;
	return (p == &dmi->errors) || (p == &oom_list) ? NULL 
				   : list_entry(p, struct dmfs_error, list);
}

static void e_stop(struct seq_file *e, void *v)
{
	struct dmfs_i *dmi = e->context;
	up(&dmi->sem);
}

static int show_error(struct seq_file *e, void *v)
{
	struct dmfs_error *d = v;
	seq_puts(e, d->msg);
	return 0;
}

static struct seq_operations error_op = {
	start: e_start,
	next: e_next,
	stop: e_stop,
	show: show_error,
};

static int dmfs_error_open(struct inode *inode, struct file *file)
{
	int ret = seq_open(file, &error_op);
	if (ret >=0) {
		struct seq_file *seq = file->private_data;
		seq->context = DMFS_I(file->f_dentry->d_parent->d_inode);
	}
	return ret;
}

static int dmfs_error_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct file_operations dmfs_error_file_operations = {
	open:		dmfs_error_open,
	read:		seq_read,
	llseek:		seq_lseek,
	release:	seq_release,
	fsync:		dmfs_error_sync,
};

static struct inode_operations dmfs_error_inode_operations = {
};

struct inode *dmfs_create_error(struct inode *dir, int mode)
{
	struct inode *inode = dmfs_new_inode(dir->i_sb, mode | S_IFREG);

	if (inode) {
		inode->i_fop = &dmfs_error_file_operations;
		inode->i_op = &dmfs_error_inode_operations;
	}

	return inode;
}

