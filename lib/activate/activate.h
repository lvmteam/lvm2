/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef ACTIVATE_H
#define ACTIVATE_H

int vg_activate(struct volume_group *vg);
int lv_activate(struct volume_group *vg, struct logical_volume *lv);

#endif
