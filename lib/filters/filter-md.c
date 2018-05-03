/*
 * Copyright (C) 2004 Luca Berra
 * Copyright (C) 2004-2006 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib.h"
#include "filter.h"

#ifdef __linux__

#define MSG_SKIPPING "%s: Skipping md component device"

static int _ignore_md(struct device *dev, int full)
{
	int ret;
	
	if (!md_filtering())
		return 1;
	
	ret = dev_is_md(dev, NULL, full);

	if (ret == -EAGAIN) {
		/* let pass, call again after scan */
		dev->flags |= DEV_FILTER_AFTER_SCAN;
		log_debug_devs("filter md deferred %s", dev_name(dev));
		return 1;
	}

	if (ret == 1) {
		if (dev->ext.src == DEV_EXT_NONE)
			log_debug_devs(MSG_SKIPPING, dev_name(dev));
		else
			log_debug_devs(MSG_SKIPPING " [%s:%p]", dev_name(dev),
					dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	if (ret < 0) {
		log_debug_devs("%s: Skipping: error in md component detection",
			       dev_name(dev));
		return 0;
	}

	return 1;
}

static int _ignore_md_lite(struct dev_filter *f __attribute__((unused)),
		           struct device *dev)
{
	return _ignore_md(dev, 0);
}

static int _ignore_md_full(struct dev_filter *f __attribute__((unused)),
		           struct device *dev)
{
	return _ignore_md(dev, 1);
}

static void _destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying md filter while in use %u times.", f->use_count);

	dm_free(f);
}

struct dev_filter *md_filter_create(struct cmd_context *cmd, struct dev_types *dt)
{
	struct dev_filter *f;

	if (!(f = dm_zalloc(sizeof(*f)))) {
		log_error("md filter allocation failed");
		return NULL;
	}

	/*
	 * FIXME: for commands that want a full md check (pvcreate, vgcreate,
	 * vgextend), we do an extra read at the end of every device that the
	 * filter looks at.  This isn't necessary; we only need to do the full
	 * md check on the PVs that these commands are trying to use.
	 */

	if (cmd->use_full_md_check)
		f->passes_filter = _ignore_md_full;
	else
		f->passes_filter = _ignore_md_lite;

	f->destroy = _destroy;
	f->use_count = 0;
	f->private = dt;

	log_debug_devs("MD filter initialised.");

	return f;
}

#else

struct dev_filter *md_filter_create(struct dev_types *dt)
{
	return NULL;
}

#endif
