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

#undef NDEBUG

#include "lvm2app.h"
#include "assert.h"

#define err(args...) \
	do { fprintf(stderr, args); goto bad; } while (0)

int main(int argc, char *argv[])
{
	lvm_t handle;
	vg_t vg;
	lv_t lv;
	int r = -1;

	if (!(handle = lvm_init(NULL)))
                return -1;

	if (!(vg = lvm_vg_open(handle, argv[1], "w", 0)))
		err("VG open %s failed.\n", argv[1]);

	if (!(lv = lvm_lv_from_name(vg, "test")))
                err("LV test not found.\n");

	if (lvm_lv_deactivate(lv))
                err("LV test deactivation failed.\n");

	if (lvm_lv_activate(lv))
                err("LV test activation failed.\n");

	if (lvm_lv_activate(lv))
                err("LV test repeated activation failed.\n");

	if (lvm_lv_rename(lv, "test1"))
		err("LV test rename to test1 failed.\n");

	if (lvm_lv_rename(lv, "test2"))
		err("LV test1 rename to test2 failed.\n");

	if (lvm_lv_rename(lv, "test"))
		err("LV test2 rename to test failed.\n");

	if (lvm_vg_close(vg))
		err("VG close failed.\n");

        r = 0;
bad:
	lvm_quit(handle);
	return r;
}
