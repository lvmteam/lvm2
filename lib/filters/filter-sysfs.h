/*
 * Copyright (C) 2004 Red Hat Inc
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_FILTER_SYSFS_H
#define _LVM_FILTER_SYSFS_H

#include "config.h"
#include "dev-cache.h"

struct dev_filter *sysfs_filter_create(const char *proc);

#endif
