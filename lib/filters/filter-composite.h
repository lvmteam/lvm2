/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_FILTER_COMPOSITE_H
#define _LVM_FILTER_COMPOSITE_H

#include "dev-cache.h"

struct dev_filter *composite_filter_create(int n, ...);

#endif
