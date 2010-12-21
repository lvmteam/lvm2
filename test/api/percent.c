/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
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

#include "lvm2app.h"

#define assert(x) do { if (!(x)) goto bad; } while (0)

int main(int argc, char *argv[])
{
	lvm_t handle;
	vg_t vg = NULL;
	lv_t lv;
	struct lvm_property_value v;

	handle = lvm_init(NULL);
        assert(handle);

	vg = lvm_vg_open(handle, argv[1], "r", 0);
        assert(vg);

	lv = lvm_lv_from_name(vg, "snap");
        assert(lv);

        v = lvm_lv_get_property(lv, "snap_percent");
        assert(v.is_valid);
        assert(v.value.integer == PERCENT_0);

        lv = lvm_lv_from_name(vg, "mirr");
        assert(lv);

        v = lvm_lv_get_property(lv, "copy_percent");
        assert(v.is_valid);
        assert(v.value.integer == PERCENT_100);

        lv = lvm_lv_from_name(vg, "snap2");
        assert(lv);

        v = lvm_lv_get_property(lv, "snap_percent");
        assert(v.is_valid);
        assert(v.value.integer == 50 * PERCENT_1);

        lvm_vg_close(vg);
        return 0;

bad:
	if (handle && lvm_errno(handle))
		fprintf(stderr, "LVM Error: %s\n", lvm_errmsg(handle));
	if (vg)
		lvm_vg_close(vg);
	if (handle)
		lvm_quit(handle);
	return 1;
}
