/*
 * Copyright (C) 1997-2004 Sistina Software, Inc. All rights reserved.  
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
#include "pool.h"
#include "label.h"
#include "metadata.h"
#include "xlate.h"
#include "disk_rep.h"
#include "pool_label.h"

#include <sys/stat.h>
#include <fcntl.h>

static void _not_supported(const char *op)
{
	log_error("The '%s' operation is not supported for the pool labeller.",
		  op);
}

static int _can_handle(struct labeller *l, char *buf, uint64_t sector)
{

	struct pool_disk pd;

	/*
	 * POOL label must always be in first sector
	 */
	if (sector)
		return 0;

	pool_label_in(&pd, buf);

	/* can ignore 8 rightmost bits for ondisk format check */
	if ((pd.pl_magic == POOL_MAGIC) &&
	    (pd.pl_version >> 8 == POOL_VERSION >> 8))
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
	struct pool_list pl;

	return read_pool_label(&pl, l, dev, buf, label);
}

static int _initialise_label(struct labeller *l, struct label *label)
{
	strcpy(label->type, "POOL");

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

struct label_ops _pool_ops = {
      can_handle:_can_handle,
      write:_write,
      read:_read,
      verify:_can_handle,
      initialise_label:_initialise_label,
      destroy_label:_destroy_label,
      destroy:_destroy
};

struct labeller *pool_labeller_create(struct format_type *fmt)
{
	struct labeller *l;

	if (!(l = dbg_malloc(sizeof(*l)))) {
		log_error("Couldn't allocate labeller object.");
		return NULL;
	}

	l->ops = &_pool_ops;
	l->private = (const void *) fmt;

	return l;
}
