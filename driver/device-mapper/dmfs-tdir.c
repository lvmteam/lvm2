/*
 * dmfs-tdir.c
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

/* Heavily based upon ramfs */

#include <linux/config.h>
#include <linux/fs.h>

#include "dm.h"

extern struct inode *dmfs_create_error(struct inode *, int);
extern struct inode *dmfs_create_table(struct inode *, int);
extern struct inode *dmfs_create_status(struct inode *, int);


int dm_lock_tdir(struct dentry *dentry)
{
	struct file file = { f_dentry: dentry };

	return deny_write_access(&file);
}

void dm_unlock_tdir(struct dentry *dentry)
{
	struct file file = { f_dentry: dentry };

	allow_write_access(&file);
}

static int dmfs_tdir_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;

	inode->i_mapping = &inode->i_data;
	inode->i_nlink--;
	dput(dentry);
	return 0;
}

static struct dentry *dmfs_tdir_lookup(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = NULL;
	const char *name = dentry->d_name.name;

	switch(dentry->d_name.len) {
		case 5:
			if (memcmp("table", name, 5) == 0)
				inode = dmfs_create_table(dir, 0600);
			if (memcmp("error", name, 5) == 0)
				inode = dmfs_create_error(dir, 0600);
			break;
		case 6:
			if (memcmp("status", name, 6) == 0)
				inode = dmfs_create_status(dir, 0600);
			break;
	}

	d_add(dentry, inode);
	return NULL;
}

static int dmfs_tdir_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	int i;
	struct dentry *dentry = filp->f_dentry;

	i = filp->f_pos;
	switch(i) {
		case 0:
			if (filldir(dirent, ".", 1, i, dentry->d_inode->i_ino, DT_DIR) < 0)
				break;
			i++;
			filp->f_pos++;
			/* fallthrough */
		case 1:
			if (filldir(dirent, "..", 2, i, dentry->d_parent->d_inode->i_ino, DT_DIR) < 0)
				break;
			i++;
			filp->f_pos++;
			/* fallthrough */
		case 2:
			if (filldir(dirent, "table", 5, i, 2, DT_REG) < 0)
				break;
			i++;
			filp->f_pos++;
			/* fallthrough */
		case 3:
			if (filldir(dirent, "error", 5, i, 3, DT_REG) < 0)
				break;
			i++;
			filp->f_pos++;
			/* fallthrough */
		case 4:
			if (filldir(dirent, "status", 6, i, 4, DT_REG) < 0)
				break;
			i++;
			filp->f_pos++;
	}
	return 0;
}


static int dmfs_tdir_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct file_operations dmfs_tdir_file_operations = {
	read:		generic_read_dir,
	readdir:	dmfs_tdir_readdir,
	fsync:		dmfs_tdir_sync,
};

static struct inode_operations dmfs_tdir_inode_operations = {
	lookup:		dmfs_tdir_lookup,
	unlink:		dmfs_tdir_unlink,
};

struct inode *dmfs_create_tdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *inode = dmfs_new_inode(dir->i_sb, mode | S_IFDIR);

	if (inode) {
		inode->i_fop = &dmfs_tdir_file_operations;
		inode->i_op = &dmfs_tdir_inode_operations;
	}

	return inode;
}


