/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "pool.h"
#include "disk-rep.h"

/*
 * FIXME: Quick hack.  We can use caching to
 * prevent a total re-read, even so vg_number
 * causes the tools to check *every* pv.  Yuck.
 * Put in separate file so it wouldn't contaminate
 * other code.
 */
int get_free_vg_number(struct format_instance *fid, struct dev_filter *filter,
		       const char *candidate_vg, int *result)
{
	struct list *pvh;
	struct list all_pvs;
	struct disk_list *dl;
	struct pool *mem = pool_create(10 * 1024);
	int numbers[MAX_VG], i, r = 0;

	list_init(&all_pvs);

	if (!mem) {
		stack;
		return 0;
	}

	if (!read_pvs_in_vg(fid->fmt, NULL, filter, mem, &all_pvs)) {
		stack;
		goto out;
	}

	memset(numbers, 0, sizeof(numbers));

	list_iterate(pvh, &all_pvs) {
		dl = list_item(pvh, struct disk_list);
		if (!*dl->pvd.vg_name || !strcmp(dl->pvd.vg_name, candidate_vg))
			continue;

		numbers[dl->vgd.vg_number] = 1;
	}

	for (i = 0; i < MAX_VG; i++) {
		if (!numbers[i]) {
			r = 1;
			*result = i;
			break;
		}
	}

      out:
	pool_destroy(mem);
	return r;
}
