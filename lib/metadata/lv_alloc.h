/*
 * Copyright (C) 2003 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_LV_ALLOC_H
#include "pool.h"

struct lv_segment *alloc_lv_segment(struct pool *mem, uint32_t num_areas);
#endif
