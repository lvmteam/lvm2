/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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
	vg_t vg = NULL;
	pv_t pv;
	struct lvm_property_value v;

	handle = lvm_init(NULL);
        assert(handle);

	vg = lvm_vg_create(handle, argv[1]);
        assert(vg);

	if (lvm_vg_extend(vg, argv[2]))
		abort();

	pv = lvm_pv_from_name(vg, argv[2]);
	assert(pv);

        v = lvm_pv_get_property(pv, "pe_start");
        assert(v.is_valid);
	fprintf(stderr, "pe_start = %d\n", (int)v.value.integer);
        assert(v.value.integer == 2048 * 512);

        lvm_vg_close(vg);
	lvm_quit(handle);
        return 0;
}
