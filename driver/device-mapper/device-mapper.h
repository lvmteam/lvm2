/*
 * device-mapper.h
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
 * Changelog
 *
 *     14/08/2001 - First version [Joe Thornber]
 */

#ifndef DEVICE_MAPPER_H
#define DEVICE_MAPPER_H

#ifdef __KERNEL__

#include <linux/major.h>

/* FIXME: Use value from local range for now, for co-existence with LVM 1 */
#define DM_BLK_MAJOR 124

struct dm_table;
typedef unsigned int offset_t;

/* constructor, destructor and map fn types */
typedef int (*dm_ctr_fn)(offset_t b, offset_t e, struct dm_table *t,
			 const char *cb, const char *ce, void **result);
typedef void (*dm_dtr_fn)(void *c);
typedef int (*dm_map_fn)(struct buffer_head *bh, void *context);

int register_map_target(const char *name, dm_ctr_fn ctr, dm_dtr_fn dtr,
			dm_map_fn map);


/* contructors should call this to make sure any
 * destination devices are handled correctly
 * (ie. opened/closed).
 */
int dm_table_add_device(struct dm_table *t, kdev_t dev);

#endif
#endif

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
