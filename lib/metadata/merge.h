/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_MERGE_H
#define _LVM_MERGE_H

#include "metadata.h"

/*
 * Sometimes (eg, after an lvextend), it is
 * possible to merge two adjacent segments into a
 * single segment.  This function trys to merge as
 * many segments as possible.
 */
int merge_segments(struct logical_volume *lv);

#endif /* _LVM_MERGE_H */
