/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef DMFS_INTERFACE_H
#define DMFS_INTERFACE_H

struct dmfs;

struct dmfs *dmfs_create(void);
void dmfs_destroy(struct dmfs *dm);

int dmfs_dev_is_present(struct dmfs *dm, const char *dev);
int dmfs_dev_is_active(struct dmfs *dm, const char *dev);

int dmfs_table_is_present(struct dmfs *dm, const char *dev, const char *table);
int dmfs_table_is_active(struct dmfs *dm, const char *dev, const char *table);

int dmfs_dev_create(struct dmfs *dm, const char *name);
int dmfs_dev_load_table(struct dmfs *dm, const char *dev, 
			const char *table, const char *file);
int dmfs_dev_drop_table(struct dmfs *dm, const char *dev, const char *table);

int dmfs_dev_activate_table(struct dmfs *dm, const char *dev,
			    const char *table);

int dmfs_dev_deactivate(struct dmfs *dm, const char *dev);

#endif
