/*
 * dmfs-dir.c
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

static int dmfs_create(struct inode *dir, struct dentry *dentry, int mode)
{
	return dmfs_create_file(dir, dentry, mode);
}

static int dmfs_unlink(struct inode *dir, struct dentry *dentry)
{

}

static struct dentry *dmfs_lookup(struct inode *dir, struct dentry *dentry)
{
	d_add(dentry, NULL);
	return NULL;
}

static int dmfs_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	if (old_dir != new_dir)
		return -EPERM;

	return 0;
}

static int dmfs_dir_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct dm_dir_file_operations = {
	read:		generic_read_dir,
	readdir:	dcache_readdir,
	fsync:		dmfs_dir_sync,
};

static struct dm_root_inode_operations = {
	create:		dmfs_dir_create,
	lookup:		dmfs_dir_lookup,
	unlink:		dmfs_dir_unlink,
	rename:		dmfs_dir_rename,
};

struct inode *dmfs_create_dir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *inode = new_inode(sb);

	if (inode) {
		inode->i_mode = mode | S_IFDIR;
		inode->i_uid = current->fsuid;
		inode->i_gid = current->fsgid;
		inode->i_blksize = PAGE_CACHE_SIZE;
		inode->i_blocks = 0;
		inode->i_rdev = NODEV;
		inode->i_atime = inode->i_ctime = inode->i_ctime = CURRENT_TIME;
		inode->i_fop = &dmfs_dir_file_operations;
		inode->i_op = &dmfs_dir_dir_operations;
	}

	return inode;
}


