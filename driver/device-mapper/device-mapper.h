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

#include <linux/major.h>

/* FIXME: steal LVM's devices for now */
#define DM_BLK_MAJOR LVM_BLK_MAJOR
#define DM_CTL_MAJOR LVM_CHAR_MAJOR

#define DM_NAME_LEN 64

typedef unsigned int offset_t;

struct dm_request {
	int minor;		/* -1 indicates no preference */
	char name[DM_NAME_LEN];
};

#define	MAPPED_DEVICE_CREATE _IOWR(DM_CTL_MAJOR, 0, struct dm_request)
#define MAPPED_DEVICE_DESTROY _IOW(DM_CTL_MAJOR, 1, struct dm_request)

#ifdef __KERNEL__
typedef int (*dm_ctr_fn)(offset_t b, offset_t e,
			 const char *context, void **result);
typedef void (*dm_dtr_fn)(void *c);
typedef int (*dm_map_fn)(struct buffer_head *bh, void *context);

int register_map_target(const char *name, dm_ctr_fn ctr, dm_dtr_fn dtr,
			dm_map_fn map);
#endif

#endif

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
