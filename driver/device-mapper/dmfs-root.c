/*
 * dmfs-root.c
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

static int is_identifier(const char *str, int len)
{
	while(len--) {
		if (!isalnum(*str) && *str != '_')
			return 0;
		str++;
	}
	return 1;
}

static int dmfs_root_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct super_block *sb = dir->i_sb;
	int rv;

	if (!is_identifier(name, dentry->d_name.len))
		return -EPERM;

	rv = dmfs_create_dir(dir, dentry, mode);
	if (rv == 0) {
		rv = dm_create(name, -1);
	}

	return rv;
}

static inline positive(struct dentry *dentry)
{
	return dentry->d_inode && ! d_unhashed(dentry);
}

static int empty(struct dentry *dentry)
{
	struct list_head *list;

	spin_lock(&dcache_lock);
	list = dentry->d_subdirs.next;

	while(list != &dentry->d_subdirs) {
		struct dentry *de = list_entry(list, struct dentry, d_child);

		if (positive(de)) {
			spin_unlock(&dcache_lock);
			return 0;
		}
		list = list->next;
	}
	spin_unlock(&dcache_lock);
	return 1;
}

static int dmfs_root_rmdir(struct inode *dir, struct dentry *dentry)
{
	int ret = -ENOTEMPTY;

	if (empty(dentry)) {
		struct inode *inode = dentry->d_inode;

		inode->i_nlink--;
		dm_remove(dentry->d_name.name);
		dput(dentry);
	}

	return retval;
}

static struct dentry *dmfs_root_lookup(struct inode *dir, struct dentry *dentry)
{
	d_add(dentry, NULL);
	return NULL;
}

static int dmfs_root_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	/* Can only rename - not move between directories! */
	if (old_dir != new_dir)
		return -EPERM;

	return -EINVAL; /* FIXME: so a change of LV name here */
}

static int dmfs_root_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct dm_root_file_operations = {
	read:		generic_read_dir,
	readdir:	dcache_readdir,
	fsync:		dmfs_root_sync,
};

static struct dm_root_inode_operations = {
	lookup:		dmfs_root_lookup,
	mkdir:		dmfs_root_mkdir,
	rmdir:		dmfs_root_rmdir,
	rename:		dmfs_root_rename,
};

struct inode *dmfs_create_root(struct super_block *sb, int mode)
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
		inode->i_fop = &dmfs_root_file_operations;
		inode->i_op = &dmfs_root_dir_operations;
	}

	return inode;
}


