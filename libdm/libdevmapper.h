/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef LIB_DEVICE_MAPPER_H
#define LIB_DEVICE_MAPPER_H

#include <inttypes.h>
#include <sys/types.h>
#include <linux/types.h>

/*
 * Since it is quite laborious to build the ioctl
 * arguments for the device-mapper people are
 * encouraged to use this library.
 *
 * You will need to build a struct dm_task for
 * each ioctl command you want to execute.
 */


typedef void (*dm_log_fn)(int level, const char *file, int line,
			  const char *f, ...);

/*
 * The library user may wish to register their own
 * logging function, by default errors go to
 * stderr.
 */
void dm_log_init(dm_log_fn fn);

enum {
	DM_DEVICE_CREATE,
	DM_DEVICE_RELOAD,
	DM_DEVICE_REMOVE,

	DM_DEVICE_SUSPEND,
	DM_DEVICE_RESUME,

	DM_DEVICE_INFO,
	DM_DEVICE_DEPS,
	DM_DEVICE_RENAME,

	DM_DEVICE_VERSION,
};


struct dm_task;

struct dm_task *dm_task_create(int type);
void dm_task_destroy(struct dm_task *dmt);

int dm_task_set_name(struct dm_task *dmt, const char *name);

/*
 * Retrieve attributes after an info.
 */
struct dm_info {
	int exists;
	int suspended;
	unsigned int open_count;
	int major;
	int minor;		/* minor device number */
	int read_only;		/* 0:read-write; 1:read-only */

	unsigned int target_count;
};

struct dm_deps {
	unsigned int count;
	__kernel_dev_t device[0];
};

int dm_get_library_version(char *version, size_t size);
int dm_task_get_driver_version(struct dm_task *dmt, char *version,
			       size_t size);
int dm_task_get_info(struct dm_task *dmt, struct dm_info *dmi);

struct dm_deps *dm_task_get_deps(struct dm_task *dmt);

int dm_task_set_ro(struct dm_task *dmt);
int dm_task_set_newname(struct dm_task *dmt, const char *newname);
int dm_task_set_minor(struct dm_task *dmt, int minor);

/*
 * Use these to prepare for a create or reload.
 */
int dm_task_add_target(struct dm_task *dmt,
		       uint64_t start,
		       uint64_t size,
		       const char *ttype,
		       const char *params);

/*
 * Call this to actually run the ioctl.
 */
int dm_task_run(struct dm_task *dmt);

/*
 * Configure the device-mapper directory
 */
int dm_set_dev_dir(const char *dir);
const char *dm_dir(void);

#endif /* LIB_DEVICE_MAPPER_H */
