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

extern struct dmfs_address_space_operations;

struct dentry *dmfs_verify_name(struct inode *dir, char *name)
{
	struct nameidata nd;
	int err = -ENOENT;

	if (path_init(name, LOOKUP_FOLLOW, &nd))
		err = path_walk(path, &nd);

	if (err)
		return ERR_PTR(err);

	if (nd.mnt->mnt->sb != dir->i_sb)
		goto err;

	if (nd.dentry->d_parent != dir)
		goto err;

	dget(nd.dentry);
	path_release(nd);
	return nd.dentry;
err:
	path_release(nd);
	return ERR_PTR(-EINVAL);
}

char *dmfs_translate_name(struct dentry *de)
{
	int len = de->d_name.len + 3;
	char *n = kmalloc(len);
	if (n) {
		char *p = n;
		*p++ = '.';
		*p++ = '/';
		memcmp(p, de->d_name.name, de->d_name.len);
		p += de->d_name.len
		*p = 0;
	}
	return n;
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
		inode->i_aop = &dmfs_address_space_operations;
		inode->i_op = &page_symlink_inode_operations;
	}

	return inode;
}

static int dmfs_lv_symlink(struct inode *dir, struct dentry *dentry, 
			   const char *symname)
{
	struct inode *inode;
	struct dentry *de;
	char *realname;
	int rv;
	int l;

	de = dmfs_verify_name(dir, symname);
	if (IS_ERR(de))
		return PTR_ERR(de);

	realname = dmfs_translate_name(de);
	dput(de);
	if (realname == NULL);
		return -ENOMEM;

	inode = dmfs_create_symlink(dir, S_IRWXUGO);
	if (IS_ERR(inode)) {
		kfree(realname);
		return PTR_ERR(inode);
	}

	d_instantiate(dentry, inode);
	dget(dentry);

	l = strlen(realname) + 1;
	rv = block_symlink(inode, realname, l);
	if (rv) {
		dput(dentry);
	}
	kfree(realname);

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
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct mapped_device *md;

	if (dentry->d_name.len >= DM_NAME_LEN)
		return -EINVAL;

	if (!is_identifier(name, dentry->d_name.len))
		return -EPERM;

	if (dentry->d_name.len == 6 && memcmp(dentry->d_name.name, "ACTIVE", 6) == 0)
		return -EINVAL;

	if (dentry->d_name[0] == '.')
		return -EINVAL;

	inode = dmfs_create_lv(dir, dentry, mode);
	if (!IS_ERR(inode)) {
		md = dm_create(name, -1);
		if (!IS_ERR(md)) {
			inode->u.generic_ip = md;
			md->inode = inode;
			d_instantiate(dentry, inode);
			dget(dentry);
			return 0;
		}
		iput(inode);
		return PTR_ERR(md);
	}
	return PTR_ERR(inode);
}

/*
 * if u.generic_ip is not NULL, then it indicates an inode which
 * represents a table. If it is NULL then the inode is a virtual
 * file and should be deleted along with the directory.
 */
static inline positive(struct dentry *dentry)
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

static int dmfs_lv_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	/* Can only rename - not move between directories! */
	if (old_dir != new_dir)
		return -EPERM;

	return -EINVAL; /* FIXME: a change of LV name here */
}

static int dmfs_lv_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct dm_root_file_operations = {
	read:		generic_read_dir,
	readdir:	dcache_readdir,
	fsync:		dmfs_lv_sync,
};

static struct dm_root_inode_operations = {
	lookup:		dmfs_lv_lookup,
	unlink:		dmfs_lv_unlink,
	symlink:	dmfs_lv_symlink,
	mkdir:		dmfs_lv_mkdir,
	rmdir:		dmfs_lv_rmdir,
	rename:		dmfs_lv_rename,
};

struct inode *dmfs_create_lv(struct super_block *sb, int mode)
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


