/*
 * Copyright (C) 2001  Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "tools.h"

static int lvscan_single(const char *vg_name);

int lvscan(int argc, char **argv)
{
	int ret;

	if (argc) {
		log_error("No additional command line arguments allowed");
		return EINVALID_CMD_LINE;
	}

	ret = process_each_vg(argc, argv, &lvscan_single);

/*********** FIXME Count!   Add private struct to process_each*  
	if (!lv_total)
		log_print("no logical volumes found");
	else {
		log_print
		    ("%d logical volumes with %s total in %d volume group%s",
		     lv_total, (dummy =
				display_size(lv_capacity_total / 2, SIZE_SHORT)),
		     vg_total, vg_total == 1 ? "" : "s");
		dbg_free(dummy);
		dummy = NULL;
		if (lv_active > 0)
			printf("%d active", lv_active);
		if (lv_active > 0 && lv_total - lv_active > 0)
			printf(" / ");
		if (lv_total - lv_active > 0)
			printf("%d inactive", lv_total - lv_active);
		printf(" logical volumes\n");
	}
*************/

	return ret;
}

static int lvscan_single(const char *vg_name)
{
	int lv_active = 0;
	int lv_total = 0;
	ulong lv_capacity_total = 0;

	int vg_total = 0;
	char *dummy;
	const char *active_str, *snapshot_str;

	struct volume_group *vg;
	struct logical_volume *lv;
	struct list_head *lvh;

	log_verbose("Checking for volume group %s", vg_name);
	if (!(vg = ios->vg_read(ios, vg_name))) {
		log_error("Volume group %s not found", vg_name);
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group %s is exported", vg_name);
		return ECMD_FAILED;
	}

	vg_total++;

	list_for_each(lvh, &vg->lvs) {
		lv = &list_entry(lvh, struct lv_list, list)->lv;

		if (lv->status & ACTIVE) {
			active_str = "ACTIVE   ";
			lv_active++;
		} else
			active_str = "inactive ";

		if (lv->status & SNAPSHOT_ORG)
			snapshot_str = "Original";
		else if (lv->status & SNAPSHOT)
			snapshot_str = "Snapshot";
		else
			snapshot_str = "        ";

/********** FIXME Snapshot
		if (lv->status & SNAPSHOT)
			dummy =
			    display_size(lv->lv_remap_end *
					 lv->lv_chunk_size / 2,
					 SIZE_SHORT);
		else
***********/
			dummy =
			    display_size(lv->size / 2, SIZE_SHORT);

		log_print("%s%s '%s%s/%s' [%s]%s%s", active_str, snapshot_str,
		       ios->prefix, vg->name, lv->name, dummy,
		       (lv->status & ALLOC_STRICT) ? " strict" : "",
		       (lv->status & ALLOC_CONTIGUOUS) ? " contiguous" : "");

                dbg_free(dummy);

		/* FIXME sprintf? */
                if (lv->stripes > 1 && !(lv->status & SNAPSHOT))
                        log_print(" striped[%u]", lv->stripes);

/******** FIXME Device number display & Snapshot
		if (arg_count(blockdevice_ARG))
			printf(" %d:%d",
			       MAJOR(lv->lv_dev),
			       MINOR(lv->lv_dev));
		else 
			if (lv->status & SNAPSHOT)
			printf(" of %s", lv->lv_snapshot_org->name);
*****************/

		lv_total++;

/******** FIXME Snapshot
		if (lv->status & SNAPSHOT)
			lv_capacity_total +=
			    lv->lv_remap_end * lv->lv_chunk_size
		else
********/
			lv_capacity_total += lv->size;
	}

	return 0;
}

