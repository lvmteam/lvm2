/*
 * Copyright (C) 2001  Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 *
 */

#include "metadata.h"
#include "dbg_malloc.h"
#include "log.h"
#include "display.h"
#include "activate.h"
#include "uuid.h"

#include <sys/types.h>
#include <string.h>

#define SIZE_BUF 128

char *display_size(unsigned long long size, size_len_t sl)
{
	int s;
	ulong byte = 1024 * 1024 * 1024;
	char *size_buf = NULL;
	char *size_str[][2] = {
		{"Terabyte", "TB"},
		{"Gigabyte", "GB"},
		{"Megabyte", "MB"},
		{"Kilobyte", "KB"},
		{"", ""}
	};

	if (!(size_buf = dbg_malloc(SIZE_BUF))) {
		log_error("no memory for size display buffer");
		return NULL;
	}

	if (size == 0LL)
		sprintf(size_buf, "0");
	else {
		s = 0;
		while (size_str[s] && size < byte)
			s++, byte /= 1024;
		snprintf(size_buf, SIZE_BUF - 1,
			 "%.2f %s", (float) size / byte, size_str[s][sl]);
	}

	/* Caller to deallocate */
	return size_buf;
}

void pvdisplay_colons(struct physical_volume *pv)
{
	char uuid[64];

	if (!pv)
		return;

	if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_print("%s:%s:%" PRIu64 ":-1:%u:%u:-1:%" PRIu64 ":%u:%u:%u:%s",
		  dev_name(pv->dev), pv->vg_name, pv->size,
		  /* FIXME pv->pv_number, Derive or remove? */
		  pv->status,	/* FIXME Support old or new format here? */
		  pv->status & ALLOCATABLE_PV,  /* FIXME remove? */
		  /* FIXME pv->lv_cur, Remove? */
		  pv->pe_size / 2,
		  pv->pe_count,
		  pv->pe_count - pv->pe_allocated,
		  pv->pe_allocated, *uuid ? uuid : "none");

	return;
}

void pvdisplay_full(struct physical_volume *pv)
{
	char uuid[64];
	char *size, *size1;	/*, *size2; */

	uint64_t pe_free;

	if (!pv)
		return;

	if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_print("--- %sPhysical volume ---", pv->pe_size ? "" : "NEW ");
	log_print("PV Name               %s", dev_name(pv->dev));
	log_print("VG Name               %s%s", pv->vg_name,
		  pv->status & EXPORTED_VG ? " (exported)" : "");

	size = display_size(pv->size / 2, SIZE_SHORT);
	if (pv->pe_size && pv->pe_count) {
		size1 = display_size((pv->size - pv->pe_count * pv->pe_size)
				     / 2, SIZE_SHORT);

/******** FIXME display LVM on-disk data size
		size2 = display_size(pv->size / 2, SIZE_SHORT);
********/

		log_print("PV Size               %s" " / not usable %s",	/*  [LVM: %s]", */
			  size, size1);	/* , size2);    */

		dbg_free(size1);
		/* dbg_free(size2); */
	} else
		log_print("PV Size               %s", size);
	dbg_free(size);

/*********FIXME Anything use this?
	log_print("PV#                   %u", pv->pv_number);
**********/

	pe_free = pv->pe_count - pv->pe_allocated;
	if (pv->pe_count && (pv->status & ALLOCATABLE_PV))
		log_print("Allocatable           yes %s",
			  (!pe_free && pv->pe_count) ? "(but full)" : "");
	else
		log_print("Allocatable           NO");

/*********FIXME
	log_print("Cur LV                %u", pv->lv_cur);
*********/
	log_print("PE Size (KByte)       %" PRIu64, pv->pe_size / 2);
	log_print("Total PE              %u", pv->pe_count);
	log_print("Free PE               %" PRIu64, pe_free);
	log_print("Allocated PE          %u", pv->pe_allocated);

#ifdef LVM_FUTURE
	printf("Stale PE              %u", pv->pe_stale);
#endif

	log_print("PV UUID               %s", *uuid ? uuid : "none");
	log_print(" ");

	return;
}

int pvdisplay_short(struct volume_group *vg, struct physical_volume *pv)
{
	if (!pv)
		return 0;

	log_print("PV Name               %s     ", dev_name(pv->dev));
	/* FIXME  pv->pv_number); */
	log_print("PV Status             %sallocatable",
		  (pv->status & ALLOCATABLE_PV) ? "" : "NOT ");
	log_print("Total PE / Free PE    %u / %u",
		  pv->pe_count, pv->pe_count - pv->pe_allocated);

	log_print(" ");
	return 0;
}

void lvdisplay_colons(struct logical_volume *lv)
{
	int inkernel;
	struct dm_info info;
	inkernel = lv_info(lv, &info) && info.exists;

	log_print("%s%s/%s:%s:%d:%d:-1:%d:%" PRIu64 ":%d:-1:%d:%d:%d:%d",
		  lv->vg->cmd->dev_dir,
		  lv->vg->name,
		  lv->name,
		  lv->vg->name,
		  (lv->status & (LVM_READ | LVM_WRITE)) >> 8,
		  inkernel ? 1 : 0,
		  /* FIXME lv->lv_number,  */
		  inkernel ? info.open_count : 0, lv->size, lv->le_count,
		  /* FIXME Add num allocated to struct! lv->lv_allocated_le, */
		  ((lv->status & ALLOC_STRICT) +
		   (lv->status & ALLOC_CONTIGUOUS) * 2), lv->read_ahead,
		  inkernel ? info.major : -1,
		  inkernel ? info.minor : -1
	    );
	return;
}

int lvdisplay_full(struct logical_volume *lv)
{
	char *size;
	uint32_t alloc;
	struct dm_info info;
	int inkernel;

	inkernel = lv_info(lv, &info) && info.exists;

	log_print("--- Logical volume ---");

	log_print("LV Name                %s%s/%s", lv->vg->cmd->dev_dir,
		  lv->vg->name, lv->name);
	log_print("VG Name                %s", lv->vg->name);

	log_print("LV Write Access        %s",
		  (lv->status & LVM_WRITE) ? "read/write" : "read only");

/******* FIXME Snapshot
    if (lv->status & (LVM_SNAPSHOT_ORG | LVM_SNAPSHOT)) {
	if (lvm_tab_vg_read_with_pv_and_lv(vg_name, &vg) < 0) {
	    ret = -LVM_ELV_SHOW_VG_READ_WITH_PV_AND_LV;
	    goto lv_show_end;
	}
	printf("LV snapshot status     ");
	if (vg_check_active(vg_name) == TRUE) {
	    vg_t *vg_core;
	    if ((ret = vg_status_with_pv_and_lv(vg_name, &vg_core)) == 0) {
		lv_t *lv_ptr =
		    vg_core->
		    lv[lv_get_index_by_name(vg_core, lv->lv_name)];
		if (lv_ptr->lv_access & LV_SNAPSHOT) {
		    if (lv_ptr->lv_status & LV_ACTIVE)
			printf("active ");
		    else
			printf("INACTIVE ");
		}
		if (lv_ptr->lv_access & LV_SNAPSHOT_ORG) {
		    printf("source of\n");
		    while (lv_ptr->lv_snapshot_next != NULL) {
			lv_ptr = lv_ptr->lv_snapshot_next;
			printf("                       %s [%s]\n",
			       lv_ptr->lv_name,
			       (lv_ptr->
				lv_status & LV_ACTIVE) ? "active" :
			       "INACTIVE");
		    }
		    vg_free(vg_core, TRUE);
		} else {
		    printf("destination for %s\n",
			   lv_ptr->lv_snapshot_org->lv_name);
		}
	    }
	} else {
	    printf("INACTIVE ");
	    if (lv->lv_access & LV_SNAPSHOT_ORG)
		printf("original\n");
	    else
		printf("snapshot\n");
	}
    }
***********/

	if (inkernel && info.suspended)
		log_print("LV Status              suspended");
	else
		log_print("LV Status              %savailable",
		  	  inkernel ? "" : "NOT ");

/********* FIXME lv_number
    log_print("LV #                   %u", lv->lv_number + 1);
************/

	if (inkernel)
		log_print("# open                 %u", info.open_count);

/********
#ifdef LVM_FUTURE
    printf("Mirror copies          %u\n", lv->lv_mirror_copies);
    printf("Consistency recovery   ");
    if (lv->lv_recovery | LV_BADBLOCK_ON)
	printf("bad blocks\n");
    else
	printf("none\n");
    printf("Schedule               %u\n", lv->lv_schedule);
#endif
********/

	size = display_size(lv->size / 2, SIZE_SHORT);
	log_print("LV Size                %s", size);
	dbg_free(size);

	log_print("Current LE             %u", lv->le_count);

/********** FIXME allocation
    log_print("Allocated LE           %u", lv->allocated_le);
**********/

/********** FIXME Snapshot
    if (lv->lv_access & LV_SNAPSHOT) {
	printf("snapshot chunk size    %s\n",
	       (dummy = lvm_show_size(lv->lv_chunk_size / 2, SHORT)));
	dbg_free(dummy);
	dummy = NULL;
	if (lv->lv_remap_end > 0) {
	    lv_remap_ptr = lv->lv_remap_ptr;
	    if (lv_remap_ptr > lv->lv_remap_end)
		lv_remap_ptr = lv->lv_remap_end;
	    dummy = lvm_show_size(lv_remap_ptr *
				  lv->lv_chunk_size / 2, SHORT);
	    dummy1 = lvm_show_size(lv->lv_remap_end *
				   lv->lv_chunk_size / 2, SHORT);
	    printf("Allocated to snapshot  %.2f%% [%s/%s]\n",
		   (float) lv_remap_ptr * 100 / lv->lv_remap_end,
		   dummy, dummy1);
	    dbg_free(dummy);
	    dbg_free(dummy1);
	    dummy =
		lvm_show_size((vg->
			       lv[lv_get_index_by_number
				  (vg,
				   lv->lv_number)]->lv_size -
			       lv->lv_remap_end * lv->lv_chunk_size) / 2,
			      SHORT);
	    printf("Allocated to COW-table %s\n", dummy);
	    dbg_free(dummy);
	}
    }
******************/

	log_print("Segments               %u", list_size(&lv->segments));

/********* FIXME Stripes & stripesize for each segment
	log_print("Stripe size (KByte)    %u", lv->stripesize / 2);
***********/

/**************
#ifdef LVM_FUTURE
    printf("Bad block             ");
    if (lv->lv_badblock == LV_BADBLOCK_ON)
	printf("on\n");
    else
	printf("off\n");
#endif
***************/

	/* FIXME next free == ALLOC_SIMPLE */
	alloc = lv->status & (ALLOC_STRICT | ALLOC_CONTIGUOUS);
	log_print("Allocation             %s%s%s%s",
		  !(alloc & (ALLOC_STRICT | ALLOC_CONTIGUOUS)) ? "next free" :
		  "", (alloc == ALLOC_STRICT) ? "strict" : "",
		  (alloc == ALLOC_CONTIGUOUS) ? "contiguous" : "",
		  (alloc ==
		   (ALLOC_STRICT | ALLOC_CONTIGUOUS)) ? "strict/contiguous" :
		  "");

	log_print("Read ahead sectors     %u", lv->read_ahead);

/****************
#ifdef LVM_FUTURE
    printf("IO Timeout (seconds)   ");
    if (lv->lv_io_timeout == 0)
	printf("default\n\n");
    else
	printf("%lu\n\n", lv->lv_io_timeout);
#endif
*************/

	if (inkernel)
    		log_print("Block device           %d:%d", info.major,
			  info.minor);

	log_print(" ");

	return 0;
}

void _display_stripe(struct stripe_segment *seg, int s, const char *pre)
{
	uint32_t len = seg->len / seg->stripes;

	log_print("%sphysical volume\t%s", pre,
		  dev_name(seg->area[s].pv->dev));

	log_print("%sphysical extents\t%d to %d",
		  pre,
		  seg->area[s].pe, seg->area[s].pe + len - 1);
}

int lvdisplay_segments(struct logical_volume *lv)
{
	int s;
	struct list *segh;
	struct stripe_segment *seg;

	log_print("--- Segments ---");

	list_iterate (segh, &lv->segments) {
		seg = list_item(segh, struct stripe_segment);

		log_print("logical extent %d to %d:",
			  seg->le, seg->le + seg->len - 1);

		if (seg->stripes == 1)
			_display_stripe(seg, 0, "  ");

		else {
			log_print("  stripes\t\t%d", seg->stripes);
			log_print("  stripe size\t\t%d", seg->stripe_size);

			for (s = 0; s < seg->stripes; s++) {
				log_print("  stripe %d:", s);
				_display_stripe(seg, s, "    ");
			}
		}
		log_print(" ");
	}

	log_print(" ");
	return 1;
}



void vgdisplay_extents(struct volume_group *vg)
{
	return;
}

void vgdisplay_full(struct volume_group *vg)
{
	uint32_t access;
	char *s1;
	char uuid[64];

	log_print("--- Volume group ---");
	log_print("VG Name               %s", vg->name);
	log_print("System ID             %s", vg->system_id);
	access = vg->status & (LVM_READ | LVM_WRITE);
	log_print("VG Access             %s%s%s%s",
		  access == (LVM_READ | LVM_WRITE) ? "read/write" : "",
		  access == LVM_READ ? "read" : "",
		  access == LVM_WRITE ? "write" : "",
		  access == 0 ? "error" : "");
	log_print("VG Status             %s%sresizable",
		  vg->status & EXPORTED_VG ? "exported/" : "",
		  vg->status & RESIZEABLE_VG ? "" : "NOT ");
/******* FIXME vg number
	log_print ("VG #                  %u\n", vg->vg_number);
********/
	if (vg->status & CLUSTERED) {
		log_print("Clustered             yes");
		log_print("Shared                %s",
			  vg->status & SHARED ? "yes" : "no");
	}
	log_print("MAX LV                %u", vg->max_lv);
	log_print("Cur LV                %u", vg->lv_count);
/****** FIXME Open LVs
      log_print ( "Open LV               %u", vg->lv_open);
*******/
/****** FIXME Max LV Size
      log_print ( "MAX LV Size           %s",
               ( s1 = display_size ( LVM_LV_SIZE_MAX(vg) / 2, SIZE_SHORT)));
      free ( s1);
*********/
	log_print("Max PV                %u", vg->max_pv);
	log_print("Cur PV                %u", vg->pv_count);
/******* FIXME act PVs
      log_print ( "Act PV                %u", vg->pv_act);
*********/

	s1 = display_size(vg->extent_count * vg->extent_size / 2, SIZE_SHORT);
	log_print("VG Size               %s", s1);
	dbg_free(s1);

	s1 = display_size(vg->extent_size / 2, SIZE_SHORT);
	log_print("PE Size               %s", s1);
	dbg_free(s1);

	log_print("Total PE              %u", vg->extent_count);

	s1 =
	    display_size((vg->extent_count - vg->free_count) *
			 vg->extent_size / 2, SIZE_SHORT);
	log_print("Alloc PE / Size       %u / %s",
		  vg->extent_count - vg->free_count, s1);
	dbg_free(s1);

	s1 = display_size(vg->free_count * vg->extent_size / 2, SIZE_SHORT);
	log_print("Free  PE / Size       %u / %s", vg->free_count, s1);
	dbg_free(s1);

	if (!id_write_format(&vg->id, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_print("VG UUID               %s", uuid);
	log_print(" ");

	return;
}

void vgdisplay_colons(struct volume_group *vg)
{
	return;
}

void vgdisplay_short(struct volume_group *vg)
{
	char *s1, *s2, *s3;
	s1 = display_size(vg->extent_count * vg->extent_size / 2, SIZE_SHORT);
	s2 =
	    display_size((vg->extent_count - vg->free_count) * vg->extent_size /
			 2, SIZE_SHORT);
	s3 = display_size(vg->free_count * vg->extent_size / 2, SIZE_SHORT);
	log_print("\"%s\" %-9s [%-9s used / %s free]", vg->name,
/********* FIXME if "open" print "/used" else print "/idle"???  ******/
		  s1, s2, s3);
	dbg_free(s1);
	dbg_free(s2);
	dbg_free(s3);
	return;
}
