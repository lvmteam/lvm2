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
	struct dm_table *table;

	if (dentry->d_name.name[0] == '.')
		return -EPERM;

	if (dentry->d_name.len == 6 && 
	    memcmp(dentry->d_name.name, "ACTIVE", 6) == 0)
		return -EPERM;

	inode = dmfs_create_table(dir, dentry, mode)
	if (!IS_ERR(inode)) {
		table = dm_table_create();
		if (table) {
			d_instantiate(dentry, inode);
			dget(dentry);
			return 0;
		}
		iput(inode);
		return -ENOMEM;
	}
	return PTR_ERR(inode);
}

static int dmfs_lv_unlink(struct inode *dir, struct dentry *dentry)
{
	inode->i_nlink--;
	dput(dentry);
	return 0;
}

static int dmfs_lv_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;

	if (!S_ISREG(inode->i_mode))
		return -EPERM;

	if (dentry->d_parent != old_dentry->d_parent)
		return -EPERM;

	if (dentry->d_name[0] == '.')
		return -EPERM;

	if (old_dentry->d_name[0] == '.')
		return -EPERM;

	inode->i_nlink++;
	atomic_inc(&inode->i_count);
	dget(dentry);
	d_instantiate(dentry, inode);

	return 0;
}

static struct dentry *dmfs_lv_lookup(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = NULL;

	if (dentry->d_name[0] == '.') {
		char *name = dentry->d_name.name;
		switch(dentry->d_name.len) {
			case 7:
				if (memcmp(".active", name, 7) == 0)
					inode = dmfs_create_active(dir, 0600);
				break;
			case 11:
				if (memcmp(".suspend_IO", name, 11) == 0)
					inode = dmfs_create_suspend(dir, 0600);
				break;
		}
	}

	d_add(dentry, inode);
	return NULL;
}

static int dmfs_lv_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	if (old_dir != new_dir)
		return -EPERM;

	if (new_dentry->d_name[0] == '.')
		return -EPERM;

	if (old_dentry->d_name[0] == '.')
		return -EPERM;

	return 0;
}

/*
 * Taken from dcache_readdir().
 * This version has a few tweeks to ensure that we always report the
 * virtual files and that we don't report them twice.
 */
static int dmfs_lv_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	int i;
	struct dentry *dentry = filp->f_dentry;

	i = flip->f_pos;
	switch(i) {
		case 0:
			if (filldir(dirent, ".", 1, i, dentry->d_inode->i_ino, DT_DIR) < 0)
				break;
			i++;
			flip->f_pos++;
			/* fallthrough */
		case 1:
			if (filldir(dirent, "..", 2, i, dentry->d_parent->d_inode->i_ino, DT_DIR) < 0
				break;
			i++;
			filp->f_pos++;
			/* fallthrough */
		case 2:
			if (filldir(dirent, ".active", 7, i, 2, DT_REG) < 0)
				break;
			i++;
			filp->f_pos++;
			/* fallthrough */
		case 3:
			if (filldir(dirent, ".suspend_IO", 11, i, 3, DT_REG) < 0)
				break;
			i++;
			filp->f_pos++;
			/* fallthrough */
		default: {
			struct list_head *list;
			int j = i - 4;

			spin_lock(&dcache_lock);
			list = dentry->d_subdirs.next;

			for(;;) {
				if (list = &dentry->d_subdirs) {
					spin_unlock(&dcache_lock);
					return 0;
				}
				if (!j)
					break;
				j--;
				list = list->next;
			}

			while(1) {
				struct dentry *de = list_entry(list, struct dentry, d_child);
				if (!d_unhashed(de) && de->d_inode && de->d_inode->generic_ip != NULL) {
					spin_unlock(&dcache_lock);
					if (filldir(dirent, de->d_name.name, de->d_name.len, filp->f_pos, de->d_inode->i_ino, DT_REG) < 0)
						break;
					spin_lock(&dcache_lock);
				}
				filp->f_pos++;
				list = list->next;
				if (list != &dentry->d_subdirs)
					continue;
				spin_unlock(&dcache_lock);
				break;
			}
		}
	}
	return 0;
}


static int dmfs_lv_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct dmfs_lv_file_operations = {
	read:		generic_read_dir,
	readdir:	dmfs_lv_readdir,
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


