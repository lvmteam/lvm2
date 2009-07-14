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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/*
 * Unit test case for vgcreate and related APIs.
 * # gcc -g vgcreate.c -I../../liblvm -I../../include -L../../liblvm \
 *   -L../../libdm -ldevmapper -llvm2app
 * # export LD_LIBRARY_PATH=`pwd`/../../libdm:`pwd`/../../liblvm
 */
#include <stdio.h>
#include <unistd.h>
#include "lvm.h"

lvm_t handle;
vg_t *vg;
char *vg_name = "my_vg";
char *device = "/dev/loop3";
uint64_t size = 1024;

int main(int argc, char *argv[])
{
	int status;

	/* FIXME: input vgname, etc from cmdline */
	/* FIXME: make the below messages verbose-only and print PASS/FAIL*/
	printf("Opening LVM\n");
	handle = lvm_create(NULL);
	if (!handle) {
		fprintf(stderr, "Unable to lvm_create\n");
		goto bad;
	}

	printf("Creating VG %s\n", vg_name);
	vg = lvm_vg_create(handle, vg_name);
	if (!vg) {
		fprintf(stderr, "Error creating volume group %s\n", vg_name);
		goto bad;
	}

	printf("Extending VG %s\n", vg_name);
	status = lvm_vg_extend(vg, device);
	if (!status) {
		fprintf(stderr, "Error extending volume group %s "
			"with device %s\n", vg_name, device);
		goto bad;
	}

	printf("Setting VG %s extent_size to %d\n", vg_name, size);
	status = lvm_vg_set_extent_size(vg, size);
	if (!status) {
		fprintf(stderr, "Can not set physical extent "
			"size '%ld' for '%s'\n",
			size, vg_name);
		goto bad;
	}

	printf("Committing VG %s to disk\n", vg_name);
	status = lvm_vg_write(vg);
	if (!status) {
		fprintf(stderr, "Creation of volume group '%s' on "
			"device '%s' failed\n",
			vg_name, device);
		goto bad;
	}

	printf("Removing VG %s from system\n", vg_name);
	status = lvm_vg_remove(vg);
	if (!status) {
		fprintf(stderr, "Revmoval of volume group '%s' failed\n",
			vg_name);
		goto bad;
	}

	lvm_vg_close(vg);
	lvm_destroy(handle);
	printf("liblvm vgcreate unit test PASS\n");
	_exit(0);
bad:
	printf("liblvm vgcreate unit test FAIL\n");
	if (vg)
		lvm_vg_close(vg);
	if (handle)
		lvm_destroy(handle);
	_exit(-1);
}
