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

/*
 * /dev/device-mapper/control is the control char device used to
 * create/destroy mapping devices.
 *
 * When a mapping device called <name> is created it appears as
 * /dev/device-mapper/<name>.  In addition the interface to control the
 * mapping will appear in /proc/device-mapper/<name>.
 */

const char *_fs_dir = "device-mapper";

static struct proc_dir_entry *_proc_dir;

static devfs_handle_t _dev_dir;
static devfs_handle_t _dev_control;

int dm_init_fs()
{
	/* create /dev/device-manager */
	_dev_dir = devfs_mk_dir(0, _fs_dir, NULL);

	/* and put the control device in it */
	_dev_control = devfs_register(0 , "device-mapper", 0, DM_CHAR_MAJOR, 0,
				      S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP,
				      &dm_ctl_fops, NULL);

	if (!(_proc_dir = create_proc_entry(_fs_dir, S_IFDIR, &proc_root))) {
		devfs_unregister(_dm_control);
		_dm_control = 0;
		return 0;
	}

	return 1;
}

void dm_fin_fs(void)
{
	/* FIXME: unregister all devices and proc interfaces */

	devfs_unregister(_dev_control);
	devfs_unregister(_dev_dir);

	remove_proc_entry(_fs_dir, &proc_root);
}

int dm_add(const char *name)
{

}

int dm_remove(const char *name)
{

}


static int _setup_targets(struct mapped_device *md, struct device_table *t)
{
	int i;
	offset_t low = 0;

	md->num_targets = t->count;
	md->targets = __aligned(sizeof(*md->targets) * md->num_targets,
			       NODE_SIZE);

	for (i = 0; i < md->num_targets; i++) {
		struct mapper *m = _find_mapper(t->map[i].type);
		if (!m)
			return 0;

		if (!m->ctr(low, t->map[i].high + 1,
			    t->map[i].context, md->contexts + i)) {
			WARN("contructor for '%s' failed", m->name);
			return 0;
		}

		md->targets[i] = m->map;
	}

	return 1;
}

static const char *_eat_white(const char *b, const char *e)
{
	while(b != e && isspace((int) *b))
		b++;

	return b;
}

static int _get_line(const char *b, const char *e,
		     const char **lb, const char **le)
{
	b = _eat_white(b, e);
	if (b == e)
		return 0;

	lb = b;
	while((b != e) && *b != '\n')
		b++;

	if (b == e) {
		/* FIXME: handle partial lines */
		return 0;
	}

	*le = b;
	return 1;
}

static int _process_line(const char *b, const char *e)
{
	struct target *t;
	const char *wb, *we;
	char buffer[64];
	int len;

	/*
	 * format of a line is:
	 * <highest valid sector> <target name> <target args>
	 */
	if (!_get_word(b, e, &wb, &we) || (we - wb) > sizeof(buffer))
		return -EPARAM;

	len = we - wb;
	strncpy(buffer, wb, we - wb);
	buffer[len] = '\0';

	if (!(t = get_target(buffer))) {
		/* FIXME: add module loading here */
		return -EPARAM;
	}

	
}

static int _write_proc(struct file *file, const char *buffer,
		       unsigned long *count, void *data)
{
	const char *b, *e, *lb, *le;
	int minor = (int) data;
	struct mapped_device *md = dm_get_dev(minor);

	if (!md)
		return -ENXIO;

	b = buffer;
	e = buffer + e;

	if (!is_loading(md)) {
		if (!_get_line(b, e, &lb, &le))
			return -EPARAM;

		if (tokcmp("begin\n", lb, le))
			return -EPARAM;

		start_loading(md);
		b = le;
	}

	while(_get_line(b, e, &lb, &le)) {
		if (!tokcmp("end\n", wb, we)) {
			/* FIXME: finish */
		}

		ret = _process_line(lb, le);
		if (ret < 0)
			goto fail;

		b = le;
	}

 fail:
	/* stop the table load */

}
