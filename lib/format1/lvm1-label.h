/*
 * Copyright (C) 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_LVM1_LABEL_H
#define _LVM_LVM1_LABEL_H

#include "metadata.h"

struct labeller *lvm1_labeller_create(struct format_type *fmt);

#endif
