/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef ACTIVATE_H
#define ACTIVATE_H

#include "dmfs-driver.h"

int lv_activate(struct dmfs *dm,
		struct volume_group *vg, struct logical_volume *lv);

int lv_deactivate(struct dmfs *dm, struct volume_group *vg,
		  struct logical_volume *lv);

#endif
