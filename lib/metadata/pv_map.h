/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_PV_MAP_H
#define _LVM_PV_MAP_H

#include "metadata.h"
#include "bitset.h"
#include "pool.h"

/*
 * The in core rep. only stores a mapping from
 * logical extents to physical extents against an
 * lv.  Sometimes, when allocating a new lv for
 * instance, it is useful to have the inverse
 * mapping available.
 */

struct pv_area {
	struct pv_map *map;
	uint32_t start;
	uint32_t count;

	struct list list;
};

struct pv_map {
	struct physical_volume *pv;
	bitset_t allocated_extents;
	struct list areas;

	struct list list;
};

struct list *create_pv_maps(struct pool *mem,
			    struct volume_group *vg, struct list *pvs);

void consume_pv_area(struct pv_area *area, uint32_t to_go);

#endif
