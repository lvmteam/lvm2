/*
 * dm.h
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
 * Internal header file for device mapper
 *
 * Changelog
 *
 *     16/08/2001 - First version [Joe Thornber]
 */

#ifndef DM_INTERNAL_H
#define DM_INTERNAL_H

#define MAX_DEPTH 16
#define NODE_SIZE L1_CACHE_BYTES
#define KEYS_PER_NODE (NODE_SIZE / sizeof(offset_t))

enum {
	DM_LOADING,
	DM_ACTIVE,
};

struct mapped_device {
	kdev_t dev;
	char name[DM_NAME_LEN];

	int state;
	atomic_t pending;

	/* btree table */
	int depth;
	int counts[MAX_DEPTH];	/* in nodes */
	offset_t *index[MAX_DEPTH];

	int num_targets;
	int num_allocated;
	offset_t *highs;
	dm_map_fn *targets;
	void **contexts;

	/* used by dm-fs.c */
	devfs_handle_t devfs_entry;
};

/* dm-target.c */
struct target {
	char *name;
	dm_ctr_fn ctr;
	dm_dtr_fn dtr;
	dm_map_fn map;

	struct target *next;
};

struct target *dm_get_target(const char *name);
int dm_std_targets(void);


/* dm.c */
struct mapped_device *dm_find_name(const char *name);
struct mapped_device *dm_find_minor(int minor);

void dm_suspend(struct mapped_device *md);
void dm_activate(struct mapped_device *md);

/* dm-table.c */
int dm_start_table(struct mapped_device *md);
int dm_add_entry(struct mapped_device *md, offset_t high,
		 dm_map_fn target, void *context);
int dm_complete_table(struct mapped_device *md);


/* dm-fs.c */
int dm_init_fs(void);
int dm_fin_fs(void);

#endif
