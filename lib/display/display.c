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

#include "lib.h"
#include "metadata.h"
#include "display.h"
#include "activate.h"
#include "toolcontext.h"

#define SIZE_BUF 128

static struct {
	alloc_policy_t alloc;
	const char *str;
} _policies[] = {
	{
	ALLOC_NEXT_FREE, "next free"}, {
	ALLOC_CONTIGUOUS, "contiguous"}, {
	ALLOC_DEFAULT, "next free (default)"}
};

static struct {
	segment_type_t segtype;
	const char *str;
} _segtypes[] = {
	{
	SEG_STRIPED, "striped"}, {
	SEG_MIRROR, "mirror"}, {
	SEG_SNAPSHOT, "snapshot"}
};

static int _num_policies = sizeof(_policies) / sizeof(*_policies);
static int _num_segtypes = sizeof(_segtypes) / sizeof(*_segtypes);

const char *get_alloc_string(alloc_policy_t alloc)
{
	int i;

	for (i = 0; i < _num_policies; i++)
		if (_policies[i].alloc == alloc)
			return _policies[i].str;

	return NULL;
}

const char *get_segtype_string(segment_type_t segtype)
{
	int i;

	for (i = 0; i < _num_segtypes; i++)
		if (_segtypes[i].segtype == segtype)
			return _segtypes[i].str;

	return NULL;
}

alloc_policy_t get_alloc_from_string(const char *str)
{
	int i;

	for (i = 0; i < _num_policies; i++)
		if (!strcmp(_policies[i].str, str))
			return _policies[i].alloc;

	log_error("Unrecognised allocation policy - using default");
	return ALLOC_DEFAULT;
}

segment_type_t get_segtype_from_string(const char *str)
{
	int i;

	for (i = 0; i < _num_segtypes; i++)
		if (!strcmp(_segtypes[i].str, str))
			return _segtypes[i].segtype;

	log_error("Unrecognised segment type - using default (striped)");
	return SEG_STRIPED;
}

char *display_size(uint64_t size, size_len_t sl)
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
		  pv->status & ALLOCATABLE_PV,	/* FIXME remove? */
		  /* FIXME pv->lv_cur, Remove? */
		  pv->pe_size / 2,
		  pv->pe_count,
		  pv->pe_count - pv->pe_alloc_count,
		  pv->pe_alloc_count, *uuid ? uuid : "none");

	return;
}

/* FIXME Include label fields */
void pvdisplay_full(struct physical_volume *pv, void *handle)
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

	size = display_size((uint64_t) pv->size / 2, SIZE_SHORT);
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

	/* PV number not part of LVM2 design
	   log_print("PV#                   %u", pv->pv_number);
	 */

	pe_free = pv->pe_count - pv->pe_alloc_count;
	if (pv->pe_count && (pv->status & ALLOCATABLE_PV))
		log_print("Allocatable           yes %s",
			  (!pe_free && pv->pe_count) ? "(but full)" : "");
	else
		log_print("Allocatable           NO");

	/* LV count is no longer available when displaying PV
	   log_print("Cur LV                %u", vg->lv_count);
	 */
	log_print("PE Size (KByte)       %" PRIu64, pv->pe_size / 2);
	log_print("Total PE              %u", pv->pe_count);
	log_print("Free PE               %" PRIu64, pe_free);
	log_print("Allocated PE          %u", pv->pe_alloc_count);
	log_print("PV UUID               %s", *uuid ? uuid : "none");
	log_print(" ");

	return;
}

int pvdisplay_short(struct cmd_context *cmd, struct volume_group *vg,
		    struct physical_volume *pv, void *handle)
{
	if (!pv)
		return 0;

	log_print("PV Name               %s     ", dev_name(pv->dev));
	/* FIXME  pv->pv_number); */
	log_print("PV Status             %sallocatable",
		  (pv->status & ALLOCATABLE_PV) ? "" : "NOT ");
	log_print("Total PE / Free PE    %u / %u",
		  pv->pe_count, pv->pe_count - pv->pe_alloc_count);

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
		  (lv->status & (LVM_READ | LVM_WRITE)) >> 8, inkernel ? 1 : 0,
		  /* FIXME lv->lv_number,  */
		  inkernel ? info.open_count : 0, lv->size, lv->le_count,
		  /* FIXME Add num allocated to struct! lv->lv_allocated_le, */
		  (lv->alloc == ALLOC_CONTIGUOUS ? 2 : 0), lv->read_ahead,
		  inkernel ? info.major : -1, inkernel ? info.minor : -1);
	return;
}

int lvdisplay_full(struct cmd_context *cmd, struct logical_volume *lv,
		   void *handle)
{
	char *size;
	struct dm_info info;
	int inkernel, snap_active;
	char uuid[64];
	struct snapshot *snap = NULL;
	struct list *slh, *snaplist;
	float snap_percent;	/* fused, fsize; */

	if (!id_write_format(&lv->lvid.id[1], uuid, sizeof(uuid))) {
		stack;
		return 0;
	}

	inkernel = lv_info(lv, &info) && info.exists;

	log_print("--- Logical volume ---");

	log_print("LV Name                %s%s/%s", lv->vg->cmd->dev_dir,
		  lv->vg->name, lv->name);
	log_print("VG Name                %s", lv->vg->name);

	log_print("LV UUID                %s", uuid);

	log_print("LV Write Access        %s",
		  (lv->status & LVM_WRITE) ? "read/write" : "read only");

	if (lv_is_origin(lv)) {
		log_print("LV snapshot status     source of");

		snaplist = find_snapshots(lv);
		list_iterate(slh, snaplist) {
			snap = list_item(slh, struct snapshot_list)->snapshot;
			snap_active = lv_snapshot_percent(snap->cow,
							  &snap_percent);
			log_print("                       %s%s/%s [%s]",
				  lv->vg->cmd->dev_dir, lv->vg->name,
				  snap->cow->name,
				  (snap_active > 0) ? "active" : "INACTIVE");
		}
	} else if ((snap = find_cow(lv))) {
		snap_active = lv_snapshot_percent(lv, &snap_percent);
		log_print("LV snapshot status     %s destination for %s%s/%s",
			  (snap_active > 0) ? "active" : "INACTIVE",
			  lv->vg->cmd->dev_dir, lv->vg->name,
			  snap->origin->name);
	}

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

	size = display_size(snap ? snap->origin->size / 2 : lv->size / 2,
			    SIZE_SHORT);
	log_print("LV Size                %s", size);
	dbg_free(size);

	log_print("Current LE             %u",
		  snap ? snap->origin->le_count : lv->le_count);

/********** FIXME allocation
    log_print("Allocated LE           %u", lv->allocated_le);
**********/

	log_print("Segments               %u", list_size(&lv->segments));

/********* FIXME Stripes & stripesize for each segment
	log_print("Stripe size (KByte)    %u", lv->stripesize / 2);
***********/

	if (snap) {
		if (snap_percent == -1)
			snap_percent = 100;

		size = display_size(snap->chunk_size / 2, SIZE_SHORT);
		log_print("Snapshot chunk size    %s", size);
		dbg_free(size);

/*
	size = display_size(lv->size / 2, SIZE_SHORT);
	sscanf(size, "%f", &fsize);
	fused = fsize * snap_percent / 100;
*/
		log_print("Allocated to snapshot  %.2f%% ",	/* [%.2f/%s]", */
			  snap_percent);	/*, fused, size); */
		/* dbg_free(size); */
	}

/********** FIXME Snapshot
	size = ???
	log_print("Allocated to COW-table %s", size);
	dbg_free(size);
    }
******************/

	log_print("Allocation             %s", get_alloc_string(lv->alloc));
	log_print("Read ahead sectors     %u", lv->read_ahead);

	if (lv->status & FIXED_MINOR)
		log_print("Persistent minor       %d", lv->minor);

	if (inkernel)
		log_print("Block device           %d:%d", info.major,
			  info.minor);

	log_print(" ");

	return 0;
}

void _display_stripe(struct lv_segment *seg, int s, const char *pre)
{
	uint32_t len = seg->len / seg->stripes;

	log_print("%sPhysical volume\t%s", pre,
		  seg->area[s].pv ? dev_name(seg->area[s].pv->dev) : "Missing");

	if (seg->area[s].pv)
		log_print("%sPhysical extents\t%d to %d", pre,
			  seg->area[s].pe, seg->area[s].pe + len - 1);
}

int lvdisplay_segments(struct logical_volume *lv)
{
	int s;
	struct list *segh;
	struct lv_segment *seg;

	log_print("--- Segments ---");

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct lv_segment);

		log_print("Logical extent %u to %u:",
			  seg->le, seg->le + seg->len - 1);

		log_print("  Type\t\t%s", get_segtype_string(seg->type));

		switch (seg->type) {
		case SEG_STRIPED:
			if (seg->stripes == 1)
				_display_stripe(seg, 0, "  ");
			else {
				log_print("  Stripes\t\t%u", seg->stripes);
				log_print("  Stripe size\t\t%u KB",
					  seg->stripe_size / 2);

				for (s = 0; s < seg->stripes; s++) {
					log_print("  Stripe %d:", s);
					_display_stripe(seg, s, "    ");
				}
			}
			log_print(" ");
			break;
		case SEG_SNAPSHOT:
		case SEG_MIRROR:
			;
		}
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
	uint32_t active_pvs;
	char *s1;
	char uuid[64];

	if (vg->status & PARTIAL_VG)
		active_pvs = list_size(&vg->pvs);
	else
		active_pvs = vg->pv_count;

	log_print("--- Volume group ---");
	log_print("VG Name               %s", vg->name);
	log_print("System ID             %s", vg->system_id);
	log_print("Format                %s", vg->fid->fmt->name);
	if (vg->fid->fmt->features & FMT_MDAS) {
		log_print("Metadata Areas        %d",
			  list_size(&vg->fid->metadata_areas));
		log_print("Metadata Sequence No  %d", vg->seqno);
	}
	access = vg->status & (LVM_READ | LVM_WRITE);
	log_print("VG Access             %s%s%s%s",
		  access == (LVM_READ | LVM_WRITE) ? "read/write" : "",
		  access == LVM_READ ? "read" : "",
		  access == LVM_WRITE ? "write" : "",
		  access == 0 ? "error" : "");
	log_print("VG Status             %s%sresizable",
		  vg->status & EXPORTED_VG ? "exported/" : "",
		  vg->status & RESIZEABLE_VG ? "" : "NOT ");
	/* vg number not part of LVM2 design
	   log_print ("VG #                  %u\n", vg->vg_number);
	 */
	if (vg->status & CLUSTERED) {
		log_print("Clustered             yes");
		log_print("Shared                %s",
			  vg->status & SHARED ? "yes" : "no");
	}
	log_print("MAX LV                %u", vg->max_lv);
	log_print("Cur LV                %u", vg->lv_count);
	log_print("Open LV               %u", lvs_in_vg_opened(vg));
/****** FIXME Max LV Size
      log_print ( "MAX LV Size           %s",
               ( s1 = display_size ( LVM_LV_SIZE_MAX(vg) / 2, SIZE_SHORT)));
      free ( s1);
*********/
	log_print("Max PV                %u", vg->max_pv);
	log_print("Cur PV                %u", vg->pv_count);
	log_print("Act PV                %u", active_pvs);

	s1 = display_size((uint64_t) vg->extent_count * (vg->extent_size / 2),
			  SIZE_SHORT);
	log_print("VG Size               %s", s1);
	dbg_free(s1);

	s1 = display_size(vg->extent_size / 2, SIZE_SHORT);
	log_print("PE Size               %s", s1);
	dbg_free(s1);

	log_print("Total PE              %u", vg->extent_count);

	s1 = display_size(((uint64_t)
			   vg->extent_count - vg->free_count) *
			  (vg->extent_size / 2), SIZE_SHORT);
	log_print("Alloc PE / Size       %u / %s",
		  vg->extent_count - vg->free_count, s1);
	dbg_free(s1);

	s1 = display_size((uint64_t) vg->free_count * (vg->extent_size / 2),
			  SIZE_SHORT);
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
	s2 = display_size((vg->extent_count -
			   vg->free_count) * vg->extent_size / 2, SIZE_SHORT);
	s3 = display_size(vg->free_count * vg->extent_size / 2, SIZE_SHORT);
	log_print("\"%s\" %-9s [%-9s used / %s free]", vg->name,
/********* FIXME if "open" print "/used" else print "/idle"???  ******/
		  s1, s2, s3);
	dbg_free(s1);
	dbg_free(s2);
	dbg_free(s3);
	return;
}
