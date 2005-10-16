/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "lvm1-label.h"
#include "disk-rep.h"
#include "label.h"
#include "metadata.h"
#include "xlate.h"
#include "lvmcache.h"

#include <sys/stat.h>
#include <fcntl.h>

static void _not_supported(const char *op)
{
	log_err("The '%s' operation is not supported for the lvm1 labeller.",
		op);
}

static int _can_handle(struct labeller *l, char *buf, uint64_t sector)
{
	struct pv_disk *pvd = (struct pv_disk *) buf;
	uint32_t version;

	/* LVM1 label must always be in first sector */
	if (sector)
		return 0;

	version = xlate16(pvd->version);

	if (pvd->id[0] == 'H' && pvd->id[1] == 'M' &&
	    (version == 1 || version == 2))
		return 1;

	return 0;
}

static int _write(struct label *label, char *buf)
{
	_not_supported("write");
	return 0;
}

static int _read(struct labeller *l, struct device *dev, char *buf,
		 struct label **label)
{
	struct pv_disk *pvd = (struct pv_disk *) buf;
	struct lvmcache_info *info;

	munge_pvd(dev, pvd);

	if (!(info = lvmcache_add(l, pvd->pv_uuid, dev, pvd->vg_name, NULL))) {
		stack;
		return 0;
	}
	*label = info->label;

	info->device_size = xlate32(pvd->pv_size) << SECTOR_SHIFT;
	list_init(&info->mdas);

	info->status &= ~CACHE_INVALID;

	return 1;
}

static int _initialise_label(struct labeller *l, struct label *label)
{
	strcpy(label->type, "LVM1");

	return 1;
}

static void _destroy_label(struct labeller *l, struct label *label)
{
	return;
}

static void _destroy(struct labeller *l)
{
	dm_free(l);
}

struct label_ops _lvm1_ops = {
	can_handle:_can_handle,
	write:_write,
	read:_read,
	verify:_can_handle,
	initialise_label:_initialise_label,
	destroy_label:_destroy_label,
	destroy:_destroy
};

struct labeller *lvm1_labeller_create(struct format_type *fmt)
{
	struct labeller *l;

	if (!(l = dm_malloc(sizeof(*l)))) {
		log_err("Couldn't allocate labeller object.");
		return NULL;
	}

	l->ops = &_lvm1_ops;
	l->private = (const void *) fmt;

	return l;
}
