/*
 * *very* heavily based on ramfs
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/file.h>

#include <asm/uaccess.h>

#include "dm.h"

/* some magic number */
#define DM_MAGIC	0x444D4653

static struct super_operations dm_ops;
static struct address_space_operations dm_aops;
static struct file_operations dm_dir_operations;
static struct file_operations dm_file_operations;
static struct inode_operations dm_dir_inode_operations;

struct vfsmount *_mnt;

static int _unlink(struct inode *dir, struct dentry *dentry);

#define NOT_A_TABLE ((struct dm_table *) 1)

/*
 * context for the line splitter and error function.
 */
struct line_c {
	unsigned int line_num;
	loff_t next_read;
	char data[MAX_TARGET_LINE];

	struct file *in;
	struct file *out;
};

static int is_identifier(const char *str, int len)
{
	while(len--) {
		if (!isalnum(*str) && *str != '_')
			return 0;
		str++;
	}

	return 1;
}

/*
 * Grabs lines one at a time from the table file.
 */
int extract_line(struct text_region *line, void *private)
{
	struct line_c *lc = (struct line_c *) private;
	struct text_region text;
	ssize_t n;
	loff_t off = lc->next_read;
	const char *read_begin;
	mm_segment_t fs;

	fs = get_fs();
	set_fs(get_ds());

	n = lc->in->f_op->read(lc->in, lc->data, sizeof(lc->data), &off);

	set_fs(fs);

	if (n <= 0)
		return 0;

	read_begin = text.b = lc->data;
	text.e = lc->data + n;

	if (!dm_get_line(&text, line))
		return 0;

	lc->line_num++;
	lc->next_read += line->e - read_begin;

	return 1;
}

static struct file *open_error_file(struct file *table)
{
	char *name;
	struct file *f;

	name = kmalloc(PATH_MAX + 1, GFP_KERNEL);

	if (!name)
		return 0;

	/* FIXME: Assumes path depth; avoid filp_open.  */
	sprintf(name, "/%s/%s/%s.err",
		table->f_vfsmnt->mnt_mountpoint->d_name.name,
		table->f_dentry->d_parent->d_name.name,
		table->f_dentry->d_name.name);
	f = filp_open(name, O_WRONLY|O_TRUNC|O_CREAT, S_IRUGO);
	kfree(name);

	if (f)
		f->f_dentry->d_inode->u.generic_ip = NOT_A_TABLE;

	return f;
}

static void close_error_file(struct file *out)
{
	fput(out);
}

static void parse_error(const char *message, void *private)
{
	char buffer[32];
	struct line_c *lc = (struct line_c *) private;

#define emit(b, l) lc->out->f_op->write(lc->out, (b), (l), &lc->out->f_pos)

	emit(lc->in->f_dentry->d_name.name, lc->in->f_dentry->d_name.len);
	sprintf(buffer, "(%d): ", lc->line_num);
	emit(buffer, strlen(buffer));
	emit(message, strlen(message));
	emit("\n", 1);

#undef emit
}

static int _release_file(struct inode *inode, struct file *f)
{
	/* FIXME: we should lock the inode to
           prevent someone else opening it while
           we are parsing */
	struct line_c *lc;
	struct dm_table *table = (struct dm_table *) inode->u.generic_ip;

	/* noop for files without tables (.err files) */
	if (table == NOT_A_TABLE)
		return 0;

	/* only bother parsing if it was open for a write */
	if (!(f->f_mode & S_IWUGO))
		return 0;

	/* free off the old table */
	if (table) {
		dm_table_destroy(table);
		inode->u.generic_ip = 0;
	}

	if (!(lc = kmalloc(sizeof(*lc), GFP_KERNEL)))
		return -ENOMEM;

	memset(lc, 0, sizeof(*lc));
	lc->in = f;

	if (!(lc->out = open_error_file(lc->in)))
		return -ENOMEM;

	table = dm_parse(extract_line, lc, parse_error, lc);

	close_error_file(lc->out);

	kfree(lc);
	inode->u.generic_ip = table;

	return 0;
}

void _release_inode(struct inode *inode)
{
	struct mapped_device *md = (struct mapped_device *)inode->u.generic_ip;
	struct dm_table *table = (struct dm_table *) inode->u.generic_ip;

	if (inode->i_mode & S_IFDIR) {
		if (md)
			dm_remove(md->name);

	} else {
		if (table)
			dm_table_destroy(table);

	}

	inode->u.generic_ip = 0;
	force_delete(inode);
}

static int _statfs(struct super_block *sb, struct statfs *buf)
{
	buf->f_type = DM_MAGIC;
	buf->f_bsize = PAGE_CACHE_SIZE;
	buf->f_namelen = 255;
	return 0;
}

/*
 * Lookup the data. This is trivial - if the dentry didn't already
 * exist, we know it is negative.
 */
static struct dentry * _lookup(struct inode *dir, struct dentry *dentry)
{
	d_add(dentry, NULL);
	return NULL;
}

/*
 * Read a page. Again trivial. If it didn't already exist
 * in the page cache, it is zero-filled.
 */
static int _readpage(struct file *file, struct page * page)
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

/*
 * Writing: just make sure the page gets marked dirty, so that
 * the page stealer won't grab it.
 */
static int _writepage(struct page *page)
{
	SetPageDirty(page);
	UnlockPage(page);
	return 0;
}

static int _prepare_write(struct file *file, struct page *page,
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

static int _commit_write(struct file *file, struct page *page,
			 unsigned offset, unsigned to)
{
	struct inode *inode = page->mapping->host;
	loff_t pos = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;

	kunmap(page);
	if (pos > inode->i_size)
		inode->i_size = pos;
	return 0;
}

struct inode *_get_inode(struct super_block *sb, int mode, int dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
		inode->i_mode = mode;
		inode->i_uid = current->fsuid;
		inode->i_gid = current->fsgid;
		inode->i_blksize = PAGE_CACHE_SIZE;
		inode->i_blocks = 0;
		inode->i_rdev = NODEV;
		inode->i_mapping->a_ops = &dm_aops;
		inode->i_atime = inode->i_mtime =
			inode->i_ctime = CURRENT_TIME;
		switch (mode & S_IFMT) {
		case S_IFBLK:
		case S_IFCHR:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_fop = &dm_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &dm_dir_inode_operations;
			inode->i_fop = &dm_dir_operations;
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			break;
		default:
			make_bad_inode(inode);
		}
	}
	return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
static int _mknod(struct inode *dir, struct dentry *dentry, int mode)
{
	int error = -ENOSPC;
	struct inode *inode = _get_inode(dir->i_sb, mode, 0);

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
	}

	return error;
}

static int _mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
	int r;
	const char *name = (const char *) dentry->d_name.name;

	if (!is_identifier(name, dentry->d_name.len))
		return -EPERM; /* or EINVAL ? */

	r = dm_create(name, -1);
	if (r)
		return r;

	r = _mknod(dir, dentry, mode | S_IFDIR);
	if (r) {
		dm_remove(name);
		return r;
	}

	dentry->d_inode->u.generic_ip = dm_find_by_name(name);

	return 0;
}

static int _rmdir(struct inode *dir, struct dentry *dentry)
{
	int r = _unlink(dir, dentry);
	if (r)
		return r;

	dm_remove(dentry->d_name.name);

	return 0;
}

static int _create(struct inode *dir, struct dentry *dentry, int mode)
{
	int r;

	if ((r = _mknod(dir, dentry, mode | S_IFREG)))
		return r;

	dentry->d_inode->u.generic_ip = 0;
	return 0;
}

static inline int positive(struct dentry *dentry)
{
	return dentry->d_inode && !d_unhashed(dentry);
}

/*
 * Check that a directory is empty (this works
 * for regular files too, they'll just always be
 * considered empty..).
 *
 * Note that an empty directory can still have
 * children, they just all have to be negative..
 */
static int _empty(struct dentry *dentry)
{
	struct list_head *list;

	spin_lock(&dcache_lock);
	list = dentry->d_subdirs.next;

	while (list != &dentry->d_subdirs) {
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

/*
 * This works for both directories and regular files.
 * (non-directories will always have empty subdirs)
 */
static int _unlink(struct inode *dir, struct dentry *dentry)
{
	int retval = -ENOTEMPTY;

	if (_empty(dentry)) {
		struct inode *inode = dentry->d_inode;

		inode->i_nlink--;
		dput(dentry);	/* Undo the count from "create" - this does all the work */
		retval = 0;
	}
	return retval;
}

/*
 * The VFS layer already does all the dentry stuff for rename,
 * we just have to decrement the usage count for the target if
 * it exists so that the VFS layer correctly free's it when it
 * gets overwritten.
 */
static int _rename(struct inode * old_dir, struct dentry *old_dentry,
		   struct inode * new_dir,struct dentry *new_dentry)
{
	struct inode *inode = new_dentry->d_inode;
	struct mapped_device *md = old_dir->u.generic_ip;
	struct dm_table *table = old_dentry->d_inode->u.generic_ip;

	if (!md || !table)
		return -EINVAL;

	if (!_empty(new_dentry))
		return -ENOTEMPTY;

	if (!strcmp(new_dentry->d_name.name, "ACTIVE")) {
		/* activate the table */
		dm_activate(md, table);

	} else if (!strcmp(old_dentry->d_name.name, "ACTIVE")) {
		dm_suspend(md);

	}

	if (inode) {
		inode->i_nlink--;
		dput(new_dentry);
	}

	return 0;
}

static int _sync_file(struct file * file, struct dentry *dentry,
		      int datasync)
{
	return 0;
}

static struct address_space_operations dm_aops = {
	readpage:	_readpage,
	writepage:	_writepage,
	prepare_write:	_prepare_write,
	commit_write:	_commit_write
};

static struct file_operations dm_file_operations = {
	read:		generic_file_read,
	write:		generic_file_write,
	mmap:		generic_file_mmap,
	fsync:		_sync_file,
	release:        _release_file,
};

static struct file_operations dm_dir_operations = {
	read:		generic_read_dir,
	readdir:	dcache_readdir,
	fsync:		_sync_file,
};

static struct inode_operations root_dir_inode_operations = {
	lookup:		_lookup,
	mkdir:		_mkdir,
	rmdir:		_rmdir,
	rename:		_rename,
};

static struct inode_operations dm_dir_inode_operations = {
	create:		_create,
	lookup:		_lookup,
	unlink:		_unlink,
	rename:		_rename,
};

static struct super_operations dm_ops = {
	statfs:		_statfs,
	put_inode:	_release_inode,
};

static struct super_block *_read_super(struct super_block * sb, void * data,
				       int silent)
{
	struct inode * inode;
	struct dentry * root;

	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = DM_MAGIC;
	sb->s_op = &dm_ops;
	inode = _get_inode(sb, S_IFDIR | 0755, 0);
	inode->i_op = &root_dir_inode_operations;
	if (!inode)
		return NULL;

	root = d_alloc_root(inode);
	if (!root) {
		iput(inode);
		return NULL;
	}
	sb->s_root = root;
	return sb;
}

static DECLARE_FSTYPE(_fs_type, "dm-fs", _read_super, FS_SINGLE);

int __init dm_fs_init(void)
{
	int r;
	if ((r = register_filesystem(&_fs_type)))
		return r;

	_mnt = kern_mount(&_fs_type);

	if (IS_ERR(_mnt)) {
		dm_fs_exit();
		return PTR_ERR(_mnt);
	}

	return 0;
}

void __exit dm_fs_exit(void)
{
	unregister_filesystem(&_fs_type);
}
