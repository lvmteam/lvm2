/*
 * dmfs-table.c
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
#include <linux/fs.h>
#include <linux/mm.h>

#include "dm.h"

static offset_t start_of_next_range(struct dm_table *t)
{
	offset_t n = 0;
	if (t->num_targets) {
		n = t->highs[t->num_targets - 1] + 1;
	}
	return n;
}

static void dmfs_parse_line(struct dm_table *t, unsigned num, char *str)
{
	char *p = str;
	const char *tok;
	offset_t start, size, high;
	void *context;
	struct target_type *ttype;
	int rv = 0;
	char *msg;

	printk("dmfs_parse_line: (%s)\n", str);

	msg = "No start argument";
	tok = next_token(&p);
	if (!tok)
		goto out;
	start = simple_strtoul(tok, NULL, 10);

	msg = "No size argument";
	tok = next_token(&p);
	if (!tok)
		goto out;
	size = simple_strtoul(tok, NULL, 10);

	msg = "Gap in table";
	if (start != start_of_next_range(t))
		goto out;

	msg = "No target type";
	tok = next_token(&p);
	if (!tok)
		goto out;

	msg = "Target type unknown";
	ttype = dm_get_target_type(tok);
	if (ttype) {
		msg = "This message should never appear (constructor error)";
		rv = ttype->ctr(t, start, size, p, &context);
		msg = context;
		if (rv == 0) {
			printk("dmfs_parse: %ul %ul %s %s\n", start, size, 
				ttype->name,
				ttype->print ? ttype->print(context) : "-");
			msg = "Error adding target to table";
			high = start + (size - 1);
			if (dm_table_add_target(t, high, ttype, context) == 0)
				return;
			ttype->dtr(t, context);
		}
		dm_put_target_type(ttype);
	}
out:
	dmfs_add_error(t, num, msg);
}


static int dmfs_copy(char *dst, int dstlen, char *src, int srclen, int *flag)
{
	int len = min(dstlen, srclen);
	char *start = dst;

	while(len) {
		*dst = *src++;
		if (*dst == '\n')
			goto end_of_line;
		dst++;
		len--;
	}
out:
	return (dst - start);
end_of_line:
	dst++;
	*flag = 1;
	goto out;
}

static int dmfs_line_is_not_comment(char *str)
{
	while(*str) {
		if (*str == '#')
			break;
		if (!isspace(*str))
			return 1;
		str++;
	}
	return 0;
}

static int dmfs_parse_page(struct dm_table *t, char *buf, int end, unsigned long end_index, char *tmp, unsigned long *tmpl, int *num)
{
	int copied;
	unsigned long len = end ? end_index : PAGE_CACHE_SIZE - 1;

	do {
		int flag = 0;
		copied = dmfs_copy(tmp + *tmpl, PAGE_SIZE - *tmpl - 1, buf, len, &flag);
		buf += copied;
		len -= copied;
		if (*tmpl + copied == PAGE_SIZE - 1)
			goto line_too_long;
		(*tmpl) += copied;
		if (flag || (len == 0 && end)) {
			*(tmp + *tmpl) = 0;
			if (dmfs_line_is_not_comment(tmp))
				dmfs_parse_line(t, *num, tmp);
			(*num)++;
			*tmpl = 0;
		}
	} while(len > 0);
	return 0;

line_too_long:
	dmfs_add_error(t, *num, "Line too long");
	/* FIXME: Add code to recover from this */
	return -1;
}

static struct dm_table *dmfs_parse(struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	unsigned long index = 0;
	unsigned long end_index, end_offset;
	unsigned long page;
	unsigned long rem = 0;
	struct dm_table *t;
	struct page *pg;
	int num = 0;

	if (inode->i_size == 0)
		return NULL;

	page = __get_free_page(GFP_KERNEL);

	if (!page)
		return NULL;

	t = dm_table_create();
	if (!t) {
		free_page(page);
		return NULL;
	}

	end_index = inode->i_size >> PAGE_CACHE_SHIFT;
	end_offset = inode->i_size & (PAGE_CACHE_SIZE - 1);

	do {
		pg = find_get_page(mapping, index);

		if (pg) {
			char *kaddr;
			int rv;

			if (!Page_Uptodate(pg))
				goto broken;

			kaddr = kmap(pg);
			rv = dmfs_parse_page(t, kaddr, (index == end_index), end_offset, (char *)page, &rem, &num);
			kunmap(pg);
			page_cache_release(pg);

			if (rv)
				goto parse_error;
		}

		index++;
	} while(index <= end_index);

	free_page(page);
	if (list_empty(&t->errors)) {
		dm_table_complete(t);
	}

	return t;

broken:
	printk(KERN_ERR "dmfs_parse: Page not uptodate\n");
	page_cache_release(pg);
	free_page(page);
	dm_table_destroy(t);
	return NULL;

parse_error:
	printk(KERN_ERR "dmfs_parse: Parse error\n");
	free_page(page);
	dm_table_destroy(t);
	return NULL;
}

static int dmfs_table_release(struct inode *inode, struct file *f)
{
	struct dentry *dentry = f->f_dentry;
	struct inode *parent = dentry->d_parent->d_inode;
	struct dmfs_i *dmi = DMFS_I(parent);
	struct dm_table *table;

	if (f->f_mode & FMODE_WRITE) {

		down(&dmi->sem);
		table = dmfs_parse(dentry->d_parent->d_inode);

		if (table) {
			if (dmi->table)
				dm_table_destroy(dmi->table);
			dmi->table = table;
		}
		up(&dmi->sem);

                put_write_access(parent);
	}

	return 0;
}

static int dmfs_readpage(struct file *file, struct page *page)
{
	if (!Page_Uptodate(page)) {
		memset(kmap(page), 0, PAGE_CACHE_SIZE);
		kunmap(page);
		flush_dcache_page(page);
		SetPageUptodate(page);
	}
	UnlockPage(page);
	return 0;
}

static int dmfs_writepage(struct page *page)
{
	SetPageDirty(page);
	UnlockPage(page);
	return 0;
}

static int dmfs_prepare_write(struct file *file, struct page *page,
			      unsigned offset, unsigned to)
{
	void *addr = kmap(page);
	if (!Page_Uptodate(page)) {
		memset(addr, 0, PAGE_CACHE_SIZE);
		flush_dcache_page(page);
		SetPageUptodate(page);
	}
	SetPageDirty(page);
	return 0;
}

static int dmfs_commit_write(struct file *file, struct page *page,
			     unsigned offset, unsigned to)
{
	struct inode *inode = page->mapping->host;
	loff_t pos = ((loff_t) page->index << PAGE_CACHE_SHIFT) + to;

	kunmap(page);
	if (pos > inode->i_size)
		inode->i_size = pos;
	return 0;
}

/*
 * There is a small race here in that two processes might call this at
 * the same time and both fail. So its a fail safe race :-) This should
 * move into namei.c (and thus use the spinlock and do this properly)
 * at some stage if we continue to use this set of functions for ensuring
 * exclusive write access to the file
 */
static int get_exclusive_write_access(struct inode *inode)
{
	if (get_write_access(inode))
		return -1;
	if (atomic_read(&inode->i_writecount) != 1) {
		put_write_access(inode);
		return -1;
	}
	return 0;
}

static int dmfs_table_open(struct inode *inode, struct file *file)
{
	struct dentry *dentry = file->f_dentry;
	struct inode *parent = dentry->d_parent->d_inode;

	if (file->f_mode & FMODE_WRITE) {
		if (get_exclusive_write_access(parent))
			return -EPERM;
	}

	return 0;
}

static int dmfs_table_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

static int dmfs_table_revalidate(struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct inode *parent = dentry->d_parent->d_inode;

	inode->i_size = parent->i_size;
	return 0;
}

struct address_space_operations dmfs_address_space_operations = {
	readpage:	dmfs_readpage,
	writepage:	dmfs_writepage,
	prepare_write:	dmfs_prepare_write,
	commit_write:	dmfs_commit_write,
};

static struct file_operations dmfs_table_file_operations = {
 	llseek:		generic_file_llseek,
	read:		generic_file_read,
	write:		generic_file_write,
	open:		dmfs_table_open,
	release:	dmfs_table_release,
	fsync:		dmfs_table_sync,
};

static struct inode_operations dmfs_table_inode_operations = {
	revalidate:	dmfs_table_revalidate,
};

struct inode *dmfs_create_table(struct inode *dir, int mode)
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
		inode->i_mapping = dir->i_mapping;
		inode->i_mapping->a_ops = &dmfs_address_space_operations;
		inode->i_fop = &dmfs_table_file_operations;
		inode->i_op = &dmfs_table_inode_operations;
	}

	return inode;
}

