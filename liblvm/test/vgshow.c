/*
 * Copyright (C) 2009 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>

#include "lvm2app.h"

int main(int argc, char *argv[])
{
	char *vgname = NULL;
	lvm_t handle;
	vg_t vg;
	lvm_str_list_t *sl;
	pv_list_t *pvl;
	lv_list_t *lvl;
	struct dm_list *vgnames;
	struct dm_list *vgids;
	struct dm_list *pvlist;
	struct dm_list *lvlist;
	uint64_t val;

	vgname = argv[1];

	handle = lvm_init(NULL);
	if (!handle) {
		printf("lvm_init failed\n");
		return -1;
	}

	vgnames = lvm_list_vg_names(handle);

	dm_list_iterate_items(sl, vgnames)
		printf("vg name %s\n", sl->str);

	vgids = lvm_list_vg_uuids(handle);

	dm_list_iterate_items(sl, vgids)
		printf("vg uuid %s\n", sl->str);

	if (!vgname) {
		printf("No vg name arg\n");
		goto out;
	}

	vg = lvm_vg_open(handle, vgname, "r", 0);

	if (!vg) {
		printf("vg open %s failed\n", vgname);
		goto out;
	}

	val = lvm_vg_get_seqno(vg);

	printf("vg seqno %llu\n", (unsigned long long)val);

	pvlist = lvm_vg_list_pvs(vg);
	
	dm_list_iterate_items(pvl, pvlist) {
		printf("vg pv name %s\n", lvm_pv_get_name(pvl->pv));

		val = lvm_pv_get_dev_size(pvl->pv);

		printf("vg pv size %llu\n", (unsigned long long)val);
	}

	lvlist = lvm_vg_list_lvs(vg);
	
	dm_list_iterate_items(lvl, lvlist) {
		printf("vg lv name %s\n", lvm_lv_get_name(lvl->lv));

		val = lvm_lv_get_size(lvl->lv);

		printf("vg lv size %llu\n", (unsigned long long)val);
	}

	lvm_vg_close(vg);
out:
	lvm_quit(handle);

	return 0;
}
