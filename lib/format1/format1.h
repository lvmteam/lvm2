/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef LVM_FORMAT1_H
#define LVM_FORMAT1_H

#include "metadata.h"

struct io_space *create_lvm_v1_format(struct dev_filter *filter);

#endif
