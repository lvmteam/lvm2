/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_FILTER_PERSISTENT_H
#define _LVM_FILTER_PERSISTENT_H

#include "dev-cache.h"

struct dev_filter *persistent_filter_create(struct dev_filter *f,
					    const char *file);

int persistent_filter_wipe(struct dev_filter *f);
int persistent_filter_load(struct dev_filter *f);
int persistent_filter_dump(struct dev_filter *f);

#endif
