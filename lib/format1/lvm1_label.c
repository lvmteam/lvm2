/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lvm1_label.h"
#include "dbg_malloc.h"
#include "disk-rep.h"
#include "log.h"
#include "label.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

static void _not_supported(const char *op)
{
	log_err("The '%s' operation is not supported for the lvm1 labeller.",
		op);
}

static int _can_handle(struct labeller *l, struct device *dev)
{
	struct pv_disk pvd;
	int r;

        if (!dev_open(dev, O_RDONLY)) {
                stack;
                return 0;
        }

	r = read_pvd(dev, &pvd);

        if (!dev_close(dev))
                stack;

	return r;
}

static int _write(struct labeller *l,
		  struct device *dev, struct label *label)
{
	_not_supported("write");
	return 0;
}

static int _remove(struct labeller *l, struct device *dev)
{
	_not_supported("remove");
	return 0;
}

static struct label *_to_label(struct pv_disk *pvd)
{
	struct label *l;
	struct lvm_label_info *info;

	if (!(l = dbg_malloc(sizeof(*l)))) {
		log_err("Couldn't allocate label.");
		return NULL;
	}

	if (!(info = (struct lvm_label_info *) dbg_strdup(pvd->vg_name))) {
		dbg_free(l);
		return NULL;
	}

	memcpy(&l->id, &pvd->pv_uuid, sizeof(l->id));
	strcpy(l->volume_type, "lvm");
	l->version[0] = 1;
	l->version[0] = 0;
	l->version[0] = 0;
	l->extra_info = info;

	return l;
}

static int _read(struct labeller *l, struct device *dev, struct label **label)
{
	struct pv_disk pvd;
	int r = 0;

        if (!dev_open(dev, O_RDONLY)) {
                stack;
                return 0;
        }

	r = read_pvd(dev, &pvd);

        if (!dev_close(dev))
                stack;

	if (!r) {
		stack;
		return 0;
	}

	/*
	 * Convert the disk_list into a label structure.
	 */
	if (!(*label = _to_label(&pvd))) {
		stack;
		return 0;
	}

	return 1;
}

static void _destroy_label(struct labeller *l, struct label *label)
{
	dbg_free(label->extra_info);
	dbg_free(label);
}

static void _destroy(struct labeller *l)
{
	dbg_free(l);
}


struct label_ops _lvm1_ops = {
	can_handle: _can_handle,
	write: _write,
	remove: _remove,
	read: _read,
	verify: _can_handle,
	destroy_label: _destroy_label,
	destroy: _destroy
};

struct labeller *lvm1_labeller_create(void)
{
	struct labeller *l;

	if (!(l = dbg_malloc(sizeof(*l)))) {
		log_err("Couldn't allocate labeller object.");
		return NULL;
	}

	l->ops = &_lvm1_ops;
	l->private = NULL;

	return l;
}
