/*
 * device-mapper.h
 *
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

/*
 * Changelog
 *
 *     14/08/2001 - First version [Joe Thornber]
 */

#ifndef DEVICE_MAPPER_H
#define DEVICE_MAPPER_H

#include <linux/major.h>

/* FIXME: Use value from local range for now, for co-existence with LVM 1 */
#define DM_BLK_MAJOR 124

struct dm_table;
struct dm_dev;
struct text_region;
typedef unsigned int offset_t;

typedef void (*dm_error_fn)(const char *message, void *private);

/*
 * constructor, destructor and map fn types
 */
typedef int (*dm_ctr_fn)(struct dm_table *t, offset_t b, offset_t l,
			 struct text_region *args, void **context,
			 dm_error_fn err, void *e_private);

typedef void (*dm_dtr_fn)(struct dm_table *t, void *c);
typedef int (*dm_map_fn)(struct buffer_head *bh, int rw, void *context);
typedef int (*dm_err_fn)(struct buffer_head *bh, int rw, void *context);


/*
 * Contructors should call this to make sure any
 * destination devices are handled correctly
 * (ie. opened/closed).
 */
struct dm_dev *dm_table_get_device(struct dm_table *table, const char *path);
void dm_table_put_device(struct dm_table *table, struct dm_dev d);

/*
 * information about a target type
 */
struct target_type {
        const char *name;
        struct module *module;
        dm_ctr_fn ctr;
        dm_dtr_fn dtr;
        dm_map_fn map;
	dm_err_fn err;
};

int dm_register_target(struct target_type *t);
int dm_unregister_target(struct target_type *t);

/*
 * These may be useful for people writing target
 * types.
 */
struct text_region {
        const char *b;
        const char *e;
};

int dm_get_number(struct text_region *txt, unsigned int *n);
int dm_get_line(struct text_region *txt, struct text_region *line);
int dm_get_word(struct text_region *txt, struct text_region *word);
void dm_txt_copy(char *dest, size_t max, struct text_region *txt);
void dm_eat_space(struct text_region *txt);


#endif /* DEVICE_MAPPER_H */

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
