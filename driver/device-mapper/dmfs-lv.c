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
#include <linux/ctype.h>
#include <linux/fs.h>

#include "dm.h"

extern struct address_space_operations dmfs_address_space_operations;
extern struct inode *dmfs_create_tdir(struct inode *dir, int mode);

struct dentry *dmfs_verify_name(struct inode *dir, const char *name)
{
	struct nameidata nd;
	int err = -ENOENT;

	if (path_init(name, LOOKUP_FOLLOW, &nd))
		return ERR_PTR(-EINVAL);

	err = path_walk(name, &nd);
	if (err)
		goto err_out;

	err = -EINVAL;
	if (nd.mnt->mnt_sb != dir->i_sb)
		goto err_out;

	if (nd.dentry->d_parent->d_inode != dir)
		goto err_out;

	dget(nd.dentry);
	path_release(&nd);
	return nd.dentry;
err_out:
	path_release(&nd);
	return ERR_PTR(err);
}

struct inode *dmfs_create_symlink(struct inode *dir, int mode)
{
	struct inode *inode = new_inode(dir->i_sb);

	if (inode) {
		inode->i_mode = mode | S_IFLNK;
		inode->i_uid = current->fsuid;
		inode->i_gid = current->fsgid;
		inode->i_blksize = PAGE_CACHE_SIZE;
		inode->i_blocks = 0;
		inode->i_rdev = NODEV;
		inode->i_atime = inode->i_ctime = inode->i_mtime = CURRENT_TIME;
		inode->i_mapping->a_ops = &dmfs_address_space_operations;
		inode->i_op = &page_symlink_inode_operations;
	}

	return inode;
}

static int dmfs_lv_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;

	if (!(inode->i_mode & S_IFLNK))
		return -EINVAL;

	inode->i_nlink--;
	dput(dentry);
	return 0;
}

static int dmfs_lv_symlink(struct inode *dir, struct dentry *dentry, 
			   const char *symname)
{
	struct inode *inode;
	struct dentry *de;
	int rv;
	int l;

	if (dentry->d_name.len != 6 || memcmp(dentry->d_name.name, "ACTIVE", 6) != 0)
		return -EINVAL;

	de = dmfs_verify_name(dir, symname);
	if (IS_ERR(de))
		return PTR_ERR(de);

	dput(de);

	inode = dmfs_create_symlink(dir, S_IRWXUGO);
	if (IS_ERR(inode)) {
		return PTR_ERR(inode);
	}

	d_instantiate(dentry, inode);
	dget(dentry);

	l = strlen(symname) + 1;
	rv = block_symlink(inode, symname, l);
	if (rv) {
		dput(dentry);
	}

	return rv;
}

static int is_identifier(const char *str, int len)
{
	while(len--) {
		if (!isalnum(*str) && *str != '_')
			return 0;
		str++;
	}
	return 1;
}

static int dmfs_lv_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *inode;

	if (dentry->d_name.len >= DM_NAME_LEN)
		return -EINVAL;

	if (!is_identifier(dentry->d_name.name, dentry->d_name.len))
		return -EPERM;

	if (dentry->d_name.len == 6 && memcmp(dentry->d_name.name, "ACTIVE", 6) == 0)
		return -EINVAL;

	if (dentry->d_name.name[0] == '.')
		return -EINVAL;

	inode = dmfs_create_tdir(dir, mode);
	if (!IS_ERR(inode)) {
		d_instantiate(dentry, inode);
		dget(dentry);
		return 0;
	}
	return PTR_ERR(inode);
}

/*
 * if u.generic_ip is not NULL, then it indicates an inode which
 * represents a table. If it is NULL then the inode is a virtual
 * file and should be deleted along with the directory.
 */
static inline int positive(struct dentry *dentry)
{
	return dentry->d_inode && !d_unhashed(dentry);
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

static int dmfs_lv_rmdir(struct inode *dir, struct dentry *dentry)
{
	int ret = -ENOTEMPTY;

	if (empty(dentry)) {
		struct inode *inode = dentry->d_inode;
		if (ret == 0) {
			inode->i_nlink--;
			dput(dentry);
		}
	}

	return ret;
}

static struct dentry *dmfs_lv_lookup(struct inode *dir, struct dentry *dentry)
{
	d_add(dentry, NULL);
	return NULL;
}

static int dmfs_lv_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct file_operations dmfs_lv_file_operations = {
	read:		generic_read_dir,
	readdir:	dcache_readdir,
	fsync:		dmfs_lv_sync,
};

static struct inode_operations dmfs_lv_inode_operations = {
	lookup:		dmfs_lv_lookup,
	unlink:		dmfs_lv_unlink,
	symlink:	dmfs_lv_symlink,
	mkdir:		dmfs_lv_mkdir,
	rmdir:		dmfs_lv_rmdir,
};

struct inode *dmfs_create_lv(struct super_block *sb, int mode, struct dentry *dentry)
{
	struct inode *inode = dmfs_new_inode(sb, mode | S_IFDIR);
	struct mapped_device *md;
	const char *name = dentry->d_name.name;

	if (inode) {
		inode->i_fop = &dmfs_lv_file_operations;
		inode->i_op = &dmfs_lv_inode_operations;
		md = dm_create(name, -1);
		if (md == NULL) {
			iput(inode);
			return NULL;
		}
		DMFS_I(inode)->md = md;
	}

	return inode;
}


