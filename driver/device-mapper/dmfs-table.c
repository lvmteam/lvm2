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
#include <linux/parser.h>
#include "dm.h"


static inline char *next_token(char **p)
{
	static const char *delim = " \t";
	char *r;

	do {
		r = strsep(p, delim);
	} while(r && *r == 0);

	return r;
}

static int dmfs_parse_line(struct dmfs_table *t, char *str)
{
	char *p = str;
	const char *delim = " \t";
	const char *tok;
	offset_t start, size, high;
	void *context;
	struct target_type *ttype;

	tok = next_token(&p);
	if (!tok)
		return -1;
	start = simple_strtoul(tok, NULL, 10);

	tok = next_token(&p);
	if (!tok)
		return -1;
	size = simple_strtoul(tok, NULL, 10);

	tok = next_token(&p);
	if (!tok)
		return -1;

	ttype = dm_get_target_type(tok);
	if (ttype) {
		context = ttype->ctr(t, start, size, p);
		if (!IS_ERR(context)) {
			high = start + (size - 1);
			if (dm_table_add_target(t, high, ttype, context) == 0)
				return 0;
			ttype->dtr(t, context);
		}
		dm_put_target_type(ttype);
	}
	return -1;
}

static int dmfs_copy(char *dst, int dstlen, char *src, int srclen, int *flag)
{
	int copied = 0;

	while(dstlen && srclen) {
		*dst = *src++;
		copied++;
		if (*dst == '\n')
			goto end_of_line;
		dst++;
		dstlen--;
		srclen--;
	}
out:
	return copied;
end_of_line:
	*flag = 1;
	*dst = 0;
	goto out;
}

static int dmfs_parse_page(struct dm_table *t, char *buf, int len, char *tmp, unsigned long *tmpl)
{
	int copied;

	do {
		int flag = 0;
		copied = dmfs_copy(tmp + *tmpl, PAGE_SIZE - *tmpl - 1, buf, len, &flag);
		buf += copied;
		len -= copied;
		if (*tmpl + copied == PAGE_SIZE - 1)
			return -1;
		*tmpl = copied;
		if (flag) {
			if (dmfs_parse_line(t, tmp))
				return -1;
			*tmpl = 0;
		}
	} while(len > 0);
	return 0;
}

static struct dm_table *dmfs_parse(struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	unsigned long index = 0;
	unsigned long offset = 0;
	unsigned long end_index, end_offset;
	unsigned long page;
	unsigned long rem = 0;
	struct dm_table *t;
	struct page *pg, **hash;

	if (inode->i_size == 0)
		return NULL;

	page = __get_free_page(GFP_KERNEL);

	if (!page)
		return NULL;

	t = dm_create_table();
	if (!t) {
		free_page(page);
		return NULL;
	}

	down(&inode->i_sem);
	end_index = inode->i_size >> PAGE_CACHE_SIZE;
	end_offset = inode->i_size & (PAGE_CAHE_SIZE - 1);

	do {
		unsigned long end = (index == end_index) ? end_offset : PAGE_CACHE_SIZE;
		hash = page_hash(mapping, index);

		spin_lock(&pagecache_lock);
		pg = __find_page_nolock(mapping, index, *hash);
		if (pg)
			page_cache_get(pg);
		spin_unlock(&pagecache_lock);

		if (pg) {
			if (!Page_Uptodate(pg))
				goto broken;

			if (dmfs_parse_page(t, pg, end, (char *)page, &rem))
				goto parse_error;
		}

		page_cache_release(pg);
		index++;
	} while(index != end_index);

	up(&inode->i_sem);

	free_page(page);
	if (dm_table_complete(t) == 0)
		return t;

	dm_put_table(t);
	return NULL;

broken:
	up(&inode->i_sem);
	printk(KERN_ERR "dmfs_parse: Page not uptodate\n");
	free_page(page);
	dm_put_table(t);
	return NULL;

parse_error:
	up(&inode->i_sem);
	printk(KERN_ERR "dmfs_parse: Parse error\n");
	free_page(page);
	dm_put_table(t);
	return NULL;
}

static int dmfs_release(struct inode *inode, struct file *f)
{
	struct dm_table *table;

	if (!(f->f_mode & S_IWUGO))
		return 0;

	table = dmfs_parse(inode);

	if (table) {
		dm_put_table((struct dm_table *)inode->u.generic_ip);
		inode->u.generic_ip = table;
	}

	return 0;
}

static int dmfs_readpage(struct file *file, struct page *page)
{
	if (!PageUptodate(page)) {
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

static int dmfs_table_sync(struct file *file, struct dentry *dentry, int datasync)
{
	return 0;
}

struct dmfs_address_space_operations = {
	readpage:	dmfs_readpage,
	writepage:	dmfs_writepage,
	prepare_write:	dmfs_prepare_write,
	commit_write:	dmfs_commit_write,
};

static struct dmfs_table_file_operations = {
 	llseek:		generic_file_llseek,
	read:		generic_file_read,
	write:		generic_file_write,
	fsync:		dmfs_table_sync,
	release:	dmfs_release,
};

static struct dmfs_table_inode_operations = {
};

int dmfs_create_table(struct inode *dir, int mode)
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
		inode->i_mapping->a_ops = &dmfs_address_space_operations;
		inode->i_fop = &dmfs_table_file_operations;
		inode->i_op = &dmfs_table_inode_operations;
	}

	return inode;
}

