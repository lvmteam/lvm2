/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_FORMAT1_H
#define _LVM_FORMAT1_H

#include "metadata.h"

struct io_space *create_lvm1_format(const char *prefix, struct pool *mem,
				    struct dev_filter *filter);

#endif
