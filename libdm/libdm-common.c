/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "libdevmapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <linux/kdev_t.h>
#include <linux/device-mapper.h>

#include "libdm-targets.h"
#include "libdm-common.h"

#define DEV_DIR "/dev/"

static char _dm_dir[PATH_MAX] = DEV_DIR DM_DIR;

/* 
 * Library users can provide their own logging
 * function.
 */
void _default_log(int level, const char *file, int line,
                         const char *f, ...)
{
        va_list ap;

        va_start(ap, f); 
        vfprintf(stderr, f, ap);
        va_end(ap);

        fprintf(stderr, "\n");
}

dm_log_fn _log = _default_log;

void dm_log_init(dm_log_fn fn)
{
        _log = fn;
}

struct dm_task *dm_task_create(int type)
{
        struct dm_task *dmt = malloc(sizeof(*dmt));

        if (!dmt) {
                log("dm_task_create: malloc(%d) failed", sizeof(*dmt));
                return NULL;
        }

        memset(dmt, 0, sizeof(*dmt));

        dmt->type = type;
        return dmt;
}

int dm_task_set_name(struct dm_task *dmt, const char *name)
{
        if (dmt->dev_name)
                free(dmt->dev_name);

        return (dmt->dev_name = strdup(name)) ? 1 : 0;
}

int dm_task_add_target(struct dm_task *dmt,
                       uint64_t start,
                       uint64_t size,
                       const char *ttype,
                       const char *params)
{
        struct target *t = create_target(start, size, ttype, params);

        if (!t)
                return 0;

        if (!dmt->head)
                dmt->head = dmt->tail = t;
        else {
                dmt->tail->next = t;
                dmt->tail = t;
        }

        return 1;
}

void _build_dev_path(char *buffer, size_t len, const char *dev_name)
{
        snprintf(buffer, len, "/dev/%s/%s", DM_DIR, dev_name);
}

int add_dev_node(const char *dev_name, dev_t dev)
{
        char path[PATH_MAX];
        struct stat info;

        _build_dev_path(path, sizeof(path), dev_name);

        if (stat(path, &info) >= 0) {
                if (!S_ISBLK(info.st_mode)) {
                        log("A non-block device file at '%s' "
                            "is already present", path);
                        return 0;
                }

                if (info.st_rdev == dev)
                        return 1;

                if (unlink(path) < 0) {
                        log("Unable to unlink device node for '%s'", dev_name);
                        return 0;
                }
        }

        if (mknod(path, S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP, dev) < 0) {
                log("Unable to make device node for '%s'", dev_name);
                return 0;
        }

        return 1;
}

int rm_dev_node(const char *dev_name)
{
        char path[PATH_MAX];
        struct stat info;

        _build_dev_path(path, sizeof(path), dev_name);

        if (stat(path, &info) < 0)
                return 1;

        if (unlink(path) < 0) {
                log("Unable to unlink device node for '%s'", dev_name);
                return 0;
        }

        return 1;
}

int dm_set_dev_dir(const char *dir)
{
        snprintf(_dm_dir, sizeof(_dm_dir), "%s%s", dir, DM_DIR);
        return 1;
}

const char *dm_dir(void)
{
        return _dm_dir;
}

