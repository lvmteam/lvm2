/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_FILTER_PERSISTENT_H
#define _LVM_FILTER_PERSISTENT_H

#include "dev-cache.h"

struct dev_filter *persistent_filter_create(const char *file, int init);

#endif
