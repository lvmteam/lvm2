/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "lvm1-label.h"
#include "disk-rep.h"
#include "label.h"
#include "metadata.h"
#include "xlate.h"
#include "cache.h"

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
	struct cache_info *info;

	if (!(info = cache_add(l, pvd->pv_uuid, dev, pvd->vg_name, NULL)))
		return 0;
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
	dbg_free(l);
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

	if (!(l = dbg_malloc(sizeof(*l)))) {
		log_err("Couldn't allocate labeller object.");
		return NULL;
	}

	l->ops = &_lvm1_ops;
	l->private = (const void *) fmt;

	return l;
}
