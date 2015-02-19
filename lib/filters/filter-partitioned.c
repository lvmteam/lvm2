/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "filter.h"

static int _passes_partitioned_filter(struct dev_filter *f, struct device *dev)
{
	struct dev_types *dt = (struct dev_types *) f->private;

	if (dev_is_partitioned(dt, dev)) {
		log_debug_devs("%s: Skipping: Partition table signature found [%s:%p]",
			       dev_name(dev), dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	return 1;
}

static void _partitioned_filter_destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying partitioned filter while in use %u times.", f->use_count);

	dm_free(f);
}

struct dev_filter *partitioned_filter_create(struct dev_types *dt)
{
	struct dev_filter *f;

	if (!(f = dm_zalloc(sizeof(struct dev_filter)))) {
		log_error("Partitioned filter allocation failed");
		return NULL;
	}

	f->passes_filter = _passes_partitioned_filter;
	f->destroy = _partitioned_filter_destroy;
	f->use_count = 0;
	f->private = dt;

	log_debug_devs("Partitioned filter initialised.");

	return f;
}
