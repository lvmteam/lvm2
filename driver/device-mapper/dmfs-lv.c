/*
 * dmfs-lv.c
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

static int dmfs_lv_create(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *inode;

	if (dentry->d_name.name[0] == '.')
		return -EPERM;

	if (dentry->d_name.len == 6 && 
	    memcmp(dentry->d_name.name, "ACTIVE", 6) == 0)
		return -EPERM;

	inode = dmfs_create_table(dir, dentry, mode)
	if (!IS_ERR(inode)) {
		d_instantiate(dentry, inode);
		dget(dentry);
		return 0;
	}
	return PTR_ERR(inode);
}

static int dmfs_lv_unlink(struct inode *dir, struct dentry *dentry)
{

}

static int dmfs_lv_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;

	if (!S_ISREG(inode->i_mode))
		return -EPERM;

	if (dentry->d_parent != old_dentry->d_parent)
		return -EPERM;

	inode->i_ctime = CURRENT_TIME;

	return 0;
}

static struct dentry *dmfs_lv_lookup(struct inode *dir, struct dentry *dentry)
{
	d_add(dentry, NULL);
	return NULL;
}

static int dmfs_lv_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	if (old_dir != new_dir)
		return -EPERM;

	return 0;
}


static int dmfs_lv_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct dmfs_lv_file_operations = {
	read:		generic_read_dir,
	readdir:	dcache_readdir,
	fsync:		dmfs_lv_sync,
};

static struct dmfs_lv_inode_operations = {
	create:		dmfs_lv_create,
	lookup:		dmfs_lv_lookup,
	link:		dmfs_lv_link,
	unlink:		dmfs_lv_unlink,
	rename:		dmfs_lv_rename,
};

struct inode *dmfs_create_lv(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *inode = new_inode(sb);

	if (inode) {
		inode->i_mode = mode | S_IFDIR;
		inode->i_uid = current->fsuid;
		inode->i_gid = current->fsgid;
		inode->i_blksize = PAGE_CACHE_SIZE;
		inode->i_blocks = 0;
		inode->i_rdev = NODEV;
		inode->i_atime = inode->i_ctime = inode->i_mtime = CURRENT_TIME;
		inode->i_fop = &dmfs_lv_file_operations;
		inode->i_op = &dmfs_lv_dir_operations;
	}

	return inode;
}


