/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_PRETTY_PRINT
#define _LVM_PRETTY_PRINT

#include "metadata.h"

#include <stdio.h>

void dump_pv(struct physical_volume *pv, FILE *fp);
void dump_lv(struct logical_volume *lv, FILE *fp);
void dump_vg(struct volume_group *vg, FILE *fp);
void dump_vg_names(struct list_head *vg_names, FILE *fp);

#endif
