/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lvm1_label.h"
#include "dbg_malloc.h"
#include "pool.h"
#include "disk-rep.h"
#include "log.h"
#include "label.h"

static void _not_supported(const char *op)
{
	log_err("The '%s' operation is not supported for the lvm1 labeller.",
		op);
}

static int _can_handle(struct labeller *l, struct device *dev)
{
	struct pool *mem = (struct pool *) l->private;
	struct disk_list *dl;

	dl = read_disk(dev, mem, NULL);
	pool_empty(mem);
	return dl ? 1 : 0;
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

static struct label *_to_label(struct disk_list *dl)
{
	struct label *l;

	if (!(l = dbg_malloc(sizeof(*l)))) {
		log_err("Couldn't allocate label.");
		return NULL;
	}

	memcpy(&l->id, &dl->pvd.pv_uuid, sizeof(l->id));
	strcpy(l->volume_type, "lvm1");
	l->version[0] = 1;
	l->version[0] = 0;
	l->version[0] = 0;

	l->extra_len = 0;
	l->extra_info = NULL;
	return l;
}

static int _read(struct labeller *l,
		 struct device *dev, struct label **label)
{
	struct pool *mem = (struct pool *) l->private;
	struct disk_list *dl;
	int r = 0;

	if (!(dl = read_disk(dev, mem, NULL))) {
		log_err("Couldn't read lvm1 label.");
		return 0;
	}

	/*
	 * Convert the disk_list into a label structure.
	 */
	if ((*label = _to_label(dl)))
		r = 1;
	else
		stack;

	pool_empty(mem);
	return r;
}

static void _destroy(struct labeller *l)
{
	struct pool *mem = (struct pool *) l->private;
	pool_destroy(mem);
	dbg_free(l->private);
}


struct label_ops _lvm1_ops = {
	can_handle: _can_handle,
	write: _write,
	remove: _remove,
	read: _read,
	verify: _can_handle,
	destroy: _destroy
};

struct labeller *lvm1_labeller_create(void)
{
	struct labeller *l;
	struct pool *mem;

	if (!(l = dbg_malloc(sizeof(*l)))) {
		log_err("Couldn't allocate labeller object.");
		return NULL;
	}

	if (!(mem = pool_create(256))) {
		log_err("Couldn't create pool object for labeller.");
		dbg_free(l);
		return NULL;
	}

	l->ops = &_lvm1_ops;
	l->private = mem;

	return l;
}
