/*
 * Copyright (C) 2003 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 * This is the in core representation of a volume group and its
 * associated physical and logical volumes.
 */

#ifndef _LVM_LV_ALLOC_H
#include "pool.h"

struct lv_segment *alloc_lv_segment(struct pool *mem, uint32_t stripes);
#endif
