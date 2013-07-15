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

#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>

#include "lvm2app.h"

lvm_t handle;
vg_t vg;

static void start() {
	handle = lvm_init(NULL);
	if (!handle) {
		fprintf(stderr, "Unable to lvm_init\n");
		abort();
	}
}

static void done(int ok) {
	if (handle && lvm_errno(handle)) {
		fprintf(stderr, "LVM Error: %s\n", lvm_errmsg(handle));
		ok = 0;
	}
	if (handle)
		lvm_quit(handle);
	if (!ok)
		abort();
}

int main(int argc, char *argv[])
{
	if (argc != 3)
		abort();

	lvm_str_list_t *str;

	int i = 0;

	start();
	struct dm_list *vgnames = lvm_list_vg_names(handle);
	dm_list_iterate_items(str, vgnames) {
		assert(++i <= 1);
		assert(!strcmp(str->str, argv[1]));
	}
	assert(i == 1);
	done(1);

	i = 0;
	start();
	struct dm_list *vgids = lvm_list_vg_uuids(handle);
	dm_list_iterate_items(str, vgids) {
		assert(++i <= 1);
		assert(!strcmp(str->str, argv[2]));
	}
	assert(i == 1);
	done(1);
	return 0;
}
