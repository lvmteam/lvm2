/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_FS_H
#define _LVM_FS_H

#include "metadata.h"

int fs_add_lv(struct io_space *ios, struct logical_volume *lv);
int fs_del_lv(struct io_space *ios, struct logical_volume *lv);

#endif
