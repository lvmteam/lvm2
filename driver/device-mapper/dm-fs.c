/*
 * dm.c
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

/*
 * procfs and devfs handling for device mapper
 *
 * Changelog
 *
 *     16/08/2001 - First version [Joe Thornber]
 */

#include "dm.h"

#include <linux/proc_fs.h>
#include <linux/ctype.h>

/*
 * /dev/device-mapper/control is the control char device used to
 * create/destroy mapping devices.
 *
 * When a mapping device called <name> is created it appears as
 * /dev/device-mapper/<name>.  In addition the interface to control the
 * mapping will appear in /proc/device-mapper/<name>.
 */

const char *_fs_dir = "device-mapper";
const char *_control_name = "control";

static struct proc_dir_entry *_proc_dir;
static struct proc_dir_entry *_control;

static devfs_handle_t _dev_dir;

static int line_splitter(struct file *file, const char *buffer,
			 unsigned long count, void *data);
static int process_control(const char *b, const char *e, int minor);
static int process_table(const char *b, const char *e, int minor);
static int get_word(const char *b, const char *e,
		    const char **wb, const char **we);
static int tok_cmp(const char *str, const char *b, const char *e);
static void tok_cpy(char *dest, size_t max,
		    const char *b, const char *e);

typedef int (*process_fn)(const char *b, const char *e, int minor);

struct pf_data {
	process_fn fn;
	int minor;
};

int dm_fs_init(void)
{
	struct pf_data *pfd = kmalloc(sizeof(*pfd), GFP_KERNEL);

	if (!pfd)
		return 0;

	_dev_dir = devfs_mk_dir(0, _fs_dir, NULL);

	if (!(_proc_dir = create_proc_entry(_fs_dir, S_IFDIR, &proc_root)))
		goto fail;

	if (!(_control = create_proc_entry(_control_name, S_IWUSR, _proc_dir)))
		goto fail;

	_control->write_proc = line_splitter;

	pfd->fn = process_control;
	pfd->minor = -1;
	_control->data = pfd;

	return 0;

 fail:
	dm_fs_exit();
	return -ENOMEM;
}

void dm_fs_exit(void)
{
	if (_control) {
		remove_proc_entry(_control_name, _proc_dir);
		_control = 0;
	}

	if (_proc_dir) {
		remove_proc_entry(_fs_dir, &proc_root);
		_proc_dir = 0;
	}

	if (_dev_dir)
		devfs_unregister(_dev_dir);
}

int dm_fs_add(struct mapped_device *md)
{
	struct pf_data *pfd = kmalloc(sizeof(*pfd), GFP_KERNEL);

	if (!pfd)
		return -ENOMEM;

	pfd->fn = process_table;
	pfd->minor = MINOR(md->dev);

	if (!(md->pde = create_proc_entry(md->name, S_IRUGO | S_IWUSR,
					_proc_dir))) {
		kfree(pfd);
		return -ENOMEM;
	}

	md->pde->write_proc = line_splitter;
	md->pde->data = pfd;

	md->devfs_entry =
		devfs_register(_dev_dir, md->name, DEVFS_FL_CURRENT_OWNER,
			       MAJOR(md->dev), MINOR(md->dev),
			       S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP,
			       &dm_blk_dops, NULL);

	if (!md->devfs_entry) {
		kfree(pfd);
		remove_proc_entry(md->name, _proc_dir);
		md->pde = 0;
		return -ENOMEM;
	}

	return 0;
}

int dm_fs_remove(struct mapped_device *md)
{
	if (md->pde) {
		kfree(md->pde->data);
		remove_proc_entry(md->name, _proc_dir);
		md->pde = 0;
	}

	devfs_unregister(md->devfs_entry);
	md->devfs_entry = 0;
	return 0;
}

static int process_control(const char *b, const char *e, int minor)
{
	const char *wb, *we;
	char name[64];
	int create = 0;

	/*
	 * create <name> [minor]
	 * remove <name>
	 */
	if (get_word(b, e, &wb, &we))
		return -EINVAL;
	b = we;

	if (!tok_cmp("create", wb, we))
		create = 1;

	else if (tok_cmp("remove", wb, we))
		return -EINVAL;

	if (get_word(b, e, &wb, &we))
		return -EINVAL;
	b = we;

	tok_cpy(name, sizeof(name), wb, we);

	if (!create)
		return dm_remove(name);

	else {
		if (!get_word(b, e, &wb, &we)) {
			minor = simple_strtol(wb, (char **) &we, 10);

			if (we == wb)
				return -EINVAL;
		}

		return dm_create(name, minor);
	}

	return -EINVAL;
}

static int process_table(const char *b, const char *e, int minor)
{
	const char *wb, *we;
	struct mapped_device *md = dm_find_by_minor(minor);
	void *context;
	int r;

	if (!md)
		return -ENXIO;

	if (get_word(b, e, &wb, &we))
		return -EINVAL;

	if (!tok_cmp("begin", b, e)) {
		/* suspend the device if it's active */
		dm_suspend(md);

		/* start loading a table */
		dm_table_start(md);

	} else if (!tok_cmp("end", b, e)) {
		/* activate the device ... <evil chuckle> ... */
		dm_table_complete(md);
		dm_activate(md);

	} else {
		/* add the new entry */
		char target[64];
		struct target_type *t;
		offset_t start, size, high;
		size_t len;

		if (get_number(&b, e, &start))
			return -EINVAL;

		if (get_number(&b, e, &size))
			return -EINVAL;

		if (get_word(b, e, &wb, &we))
			return -EINVAL;

		len = we - wb;
		if (len > sizeof(target))
			return -EINVAL;

		strncpy(target, wb, len);
		target[len] = '\0';

		if (!(t = dm_get_target(target)))
			return -EINVAL;

		/* check there isn't a gap */
		if ((md->num_targets &&
		     start != md->highs[md->num_targets - 1] + 1) ||
		    (!md->num_targets && start)) {
			WARN("gap in target ranges");
			return -EINVAL;
		}

		high = start + (size - 1);
		if ((r = t->ctr(start, high, md, we, e, &context)))
			return r;

		if ((r = dm_table_add_entry(md, high, t->map, context)))
			return r;
	}

	return 0;
}

static int get_word(const char *b, const char *e,
		    const char **wb, const char **we)
{
	b = eat_space(b, e);

	if (b == e)
		return -EINVAL;

	*wb = b;
	while(b != e && !isspace((int) *b))
		b++;
	*we = b;
	return 0;
}

static int line_splitter(struct file *file, const char *buffer,
			 unsigned long count, void *data)
{
	int r;
	const char *b = buffer, *e = buffer + count, *lb;
	struct pf_data *pfd = (struct pf_data *) data;

	while(b < e) {
		b = eat_space(b, e);
		if (b == e)
			break;

		lb = b;
		while((b != e) && *b != '\n')
			b++;

		if ((r = pfd->fn(lb, b, pfd->minor)))
			return r;
	}

	return count;
}

static int tok_cmp(const char *str, const char *b, const char *e)
{
	while (*str && b != e) {
		if (*str < *b)
			return -1;

		if (*str > *b)
			return 1;

		str++, b++;
	}

	if (!*str && b == e)
		return 0;

	if (*str)
		return 1;

	return -1;
}

static void tok_cpy(char *dest, size_t max,
		    const char *b, const char *e)
{
	size_t len = e - b;
	if (len > --max)
		len = max;
	strncpy(dest, b, len);
	dest[len] = '\0';
}
