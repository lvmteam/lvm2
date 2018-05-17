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
	lv_t lv;
	lvm_str_list_t *sl;
	pv_list_t *pvl;
	lv_list_t *lvl;
	struct dm_list *vgnames;
	struct dm_list *vgids;
	struct dm_list *pvlist;
	struct dm_list *lvlist;
	int added = 0;
	int ret;
	int i;

	vgname = argv[1];

	handle = lvm_init(NULL);
	if (!handle) {
		printf("lvm_init failed\n");
		return -1;
	}

	vg = lvm_vg_create(handle, vgname);

	for (i = 2; i < argc; i++) {
		printf("adding %s to vg\n", argv[i]);
		ret = lvm_vg_extend(vg, argv[i]);

		if (ret) {
			printf("Failed to add %s to vg\n", argv[i]);
			goto out;
		}

		added++;
	}

	if (!added) {
		printf("No PVs added, not writing VG.\n");
		goto out;
	}

	printf("writing vg\n");
	ret = lvm_vg_write(vg);

	lvm_vg_close(vg);

	sleep(1);

	vg = lvm_vg_open(handle, vgname, "w", 0);
	if (!vg) {
		printf("vg open %s failed\n", vgname);
		goto out;
	}

	lv = lvm_vg_create_lv_linear(vg, "lv0", 1024*1024);
	if (!lv) {
		printf("lv create failed\n");
		goto out;
	}

	lvm_vg_close(vg);
out:
	lvm_quit(handle);

	return 0;
}
