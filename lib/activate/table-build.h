/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef TABLE_BUILD_H
#define TABLE_BUILD_H

int build_table(struct volume_group *vg, struct logical_volume *lv,
		const char *file);

#endif
