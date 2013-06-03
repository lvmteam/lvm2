/*
 * Copyright (C) 2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#undef NDEBUG

#include "lvm2app.h"
#include "assert.h"

int main(int argc, char *argv[])
{
	lvm_t handle;
	vg_t vg;
	lv_t lv;
	struct lvm_property_value v;
	struct dm_list *lvsegs;
	struct lvm_lvseg_list *lvl;

	handle = lvm_init(NULL);
	assert(handle);

	vg = lvm_vg_open(handle, argv[1], "r", 0);
	assert(vg);

	lv = lvm_lv_from_name(vg, "pool");
	assert(lv);

	lvsegs = lvm_lv_list_lvsegs(lv);
	assert(lvsegs && (dm_list_size(lvsegs) == 1));
	dm_list_iterate_items(lvl, lvsegs) {
		v = lvm_lvseg_get_property(lvl->lvseg, "discards");
		assert(v.is_valid && v.is_string);
		assert(strcmp(v.value.string, "passdown") == 0);
	}

	v = lvm_lv_get_property(lv, "data_percent");
	assert(v.is_valid);
	assert(v.value.integer == 25 * PERCENT_1);


	lv = lvm_lv_from_name(vg, "thin");
	assert(lv);

	v = lvm_lv_get_property(lv, "data_percent");
	assert(v.is_valid);
	assert(v.value.integer == 50 * PERCENT_1);


	lv = lvm_lv_from_name(vg, "snap");
	assert(lv);

	v = lvm_lv_get_property(lv, "data_percent");
	assert(v.is_valid);
	assert(v.value.integer == 75 * PERCENT_1);

	v = lvm_lv_get_property(lv, "snap_percent");
	assert(v.is_valid);
	assert(v.value.integer == (uint64_t) PERCENT_INVALID);

	v = lvm_lv_get_property(lv, "origin");
	assert(v.is_valid);
	assert(strcmp(v.value.string, "thin") == 0);

	lvm_vg_close(vg);
	lvm_quit(handle);

	return 0;
}
