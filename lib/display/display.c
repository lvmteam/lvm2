/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "metadata.h"
#include "display.h"
#include "activate.h"
#include "toolcontext.h"
#include "segtypes.h"

#define SIZE_BUF 128

static struct {
	alloc_policy_t alloc;
	const char *str;
} _policies[] = {
	{
	ALLOC_CONTIGUOUS, "contiguous"}, {
	ALLOC_NORMAL, "normal"}, {
	ALLOC_ANYWHERE, "anywhere"}, {
	ALLOC_INHERIT, "inherit"}
};

static int _num_policies = sizeof(_policies) / sizeof(*_policies);

uint64_t units_to_bytes(const char *units, char *unit_type)
{
	char *ptr = NULL;
	uint64_t v;

	if (isdigit(*units)) {
		v = (uint64_t) strtod(units, &ptr);
		if (ptr == units)
			return 0;
		units = ptr;
	} else
		v = 1;

	if (v == 1)
		*unit_type = *units;
	else
		*unit_type = 'U';

	switch (*units) {
	case 'h':
	case 'H':
		v = UINT64_C(1);
		*unit_type = *units;
		break;
	case 's':
		v *= SECTOR_SIZE;
		break;
	case 'b':
	case 'B':
		v *= UINT64_C(1);
		break;
#define KILO UINT64_C(1024)
	case 'k':
		v *= KILO;
		break;
	case 'm':
		v *= KILO * KILO;
		break;
	case 'g':
		v *= KILO * KILO * KILO;
		break;
	case 't':
		v *= KILO * KILO * KILO * KILO;
		break;
#undef KILO
#define KILO UINT64_C(1000)
	case 'K':
		v *= KILO;
		break;
	case 'M':
		v *= KILO * KILO;
		break;
	case 'G':
		v *= KILO * KILO * KILO;
		break;
	case 'T':
		v *= KILO * KILO * KILO * KILO;
		break;
#undef KILO
	default:
		return 0;
	}

	if (*(units + 1))
		return 0;

	return v;
}

const char *get_alloc_string(alloc_policy_t alloc)
{
	int i;

	for (i = 0; i < _num_policies; i++)
		if (_policies[i].alloc == alloc)
			return _policies[i].str;

	return NULL;
}

alloc_policy_t get_alloc_from_string(const char *str)
{
	int i;

	for (i = 0; i < _num_policies; i++)
		if (!strcmp(_policies[i].str, str))
			return _policies[i].alloc;

	/* Special case for old metadata */
	if(!strcmp("next free", str))
		return ALLOC_NORMAL;

	log_error("Unrecognised allocation policy %s", str);
	return ALLOC_INVALID;
}

/* Size supplied in sectors */
const char *display_size(struct cmd_context *cmd, uint64_t size, size_len_t sl)
{
	int s;
	int suffix = 1, precision;
	uint64_t byte = UINT64_C(0);
	uint64_t units = UINT64_C(1024);
	char *size_buf = NULL;
	const char *size_str[][3] = {
		{" Terabyte", " TB", "T"},
		{" Gigabyte", " GB", "G"},
		{" Megabyte", " MB", "M"},
		{" Kilobyte", " KB", "K"},
		{"", "", ""},
		{" Byte    ", " B ", "B"},
		{" Units   ", " Un", "U"},
		{" Sectors ", " Se", "S"},
		{"         ", "   ", " "},
	};

	if (!(size_buf = pool_alloc(cmd->mem, SIZE_BUF))) {
		log_error("no memory for size display buffer");
		return "";
	}

	suffix = cmd->current_settings.suffix;

	for (s = 0; s < 8; s++)
		if (toupper((int) cmd->current_settings.unit_type) ==
		    *size_str[s][2])
			break;

	if (size == UINT64_C(0)) {
		sprintf(size_buf, "0%s", suffix ? size_str[s][sl] : "");
		return size_buf;
	}

	if (s < 8) {
		byte = cmd->current_settings.unit_factor;
		size *= UINT64_C(512);
	} else {
		size /= 2;
		suffix = 1;
		if (cmd->current_settings.unit_type == 'H')
			units = UINT64_C(1000);
		else
			units = UINT64_C(1024);
		byte = units * units * units;
		s = 0;
		while (size_str[s] && size < byte)
			s++, byte /= units;
	}

	/* FIXME Make precision configurable */
	switch(toupper((int) cmd->current_settings.unit_type)) {
	case 'B':
	case 'S':
		precision = 0;
		break;
	default:
		precision = 2;
	}

	snprintf(size_buf, SIZE_BUF - 1, "%.*f%s", precision,
		 (double) size / byte, suffix ? size_str[s][sl] : "");

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

	log_print("%s:%s:%" PRIu64 ":-1:%u:%u:-1:%" PRIu32 ":%u:%u:%u:%s",
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
void pvdisplay_full(struct cmd_context *cmd, struct physical_volume *pv,
		    void *handle)
{
	char uuid[64];
	const char *size;

	uint32_t pe_free;

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

	size = display_size(cmd, (uint64_t) pv->size, SIZE_SHORT);
	if (pv->pe_size && pv->pe_count) {

/******** FIXME display LVM on-disk data size
		size2 = display_size(pv->size, SIZE_SHORT);
********/

		log_print("PV Size               %s" " / not usable %s",	/*  [LVM: %s]", */
			  size,
			  display_size(cmd, (pv->size -
					     pv->pe_count * pv->pe_size),
				       SIZE_SHORT));

	} else
		log_print("PV Size               %s", size);

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
	log_print("PE Size (KByte)       %" PRIu32, pv->pe_size / 2);
	log_print("Total PE              %u", pv->pe_count);
	log_print("Free PE               %" PRIu32, pe_free);
	log_print("Allocated PE          %u", pv->pe_alloc_count);
	log_print("PV UUID               %s", *uuid ? uuid : "none");
	log_print(" ");

	return;
}

int pvdisplay_short(struct cmd_context *cmd, struct volume_group *vg,
		    struct physical_volume *pv, void *handle)
{
	char uuid[64];

	if (!pv)
		return 0;

	if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
		stack;
		return 0;
	}

	log_print("PV Name               %s     ", dev_name(pv->dev));
	/* FIXME  pv->pv_number); */
	log_print("PV UUID               %s", *uuid ? uuid : "none");
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
	struct lvinfo info;
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
	struct lvinfo info;
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
			if (!snap_active || snap_percent < 0 ||
			    snap_percent >= 100) snap_active = 0;
			log_print("                       %s%s/%s [%s]",
				  lv->vg->cmd->dev_dir, lv->vg->name,
				  snap->cow->name,
				  (snap_active > 0) ? "active" : "INACTIVE");
		}
		snap = NULL;
	} else if ((snap = find_cow(lv))) {
		snap_active = lv_snapshot_percent(lv, &snap_percent);
		if (!snap_active || snap_percent < 0 || snap_percent >= 100)
			snap_active = 0;
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

	log_print("LV Size                %s",
		  display_size(cmd,
			       snap ? snap->origin->size : lv->size,
			       SIZE_SHORT));

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

		log_print("Snapshot chunk size    %s",
			  display_size(cmd, (uint64_t) snap->chunk_size,
				       SIZE_SHORT));

/*
	size = display_size(lv->size, SIZE_SHORT);
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

	if (lv->status & FIXED_MINOR) {
		if (lv->major >= 0)
			log_print("Persistent major       %d", lv->major);
		log_print("Persistent minor       %d", lv->minor);
	}

	if (inkernel)
		log_print("Block device           %d:%d", info.major,
			  info.minor);

	log_print(" ");

	return 0;
}

void display_stripe(const struct lv_segment *seg, uint32_t s, const char *pre)
{
	switch (seg->area[s].type) {
	case AREA_PV:
		log_print("%sPhysical volume\t%s", pre,
			  seg->area[s].u.pv.pv ?
			  dev_name(seg->area[s].u.pv.pv->dev) : "Missing");

		if (seg->area[s].u.pv.pv)
			log_print("%sPhysical extents\t%d to %d", pre,
				  seg->area[s].u.pv.pe,
				  seg->area[s].u.pv.pe + seg->area_len - 1);
		break;
	case AREA_LV:
		log_print("%sLogical volume\t%s", pre,
			  seg->area[s].u.lv.lv ?
			  seg->area[s].u.lv.lv->name : "Missing");

		if (seg->area[s].u.lv.lv)
			log_print("%sLogical extents\t%d to %d", pre,
				  seg->area[s].u.lv.le,
				  seg->area[s].u.lv.le + seg->area_len - 1);

	}
}

int lvdisplay_segments(struct logical_volume *lv)
{
	struct lv_segment *seg;

	log_print("--- Segments ---");

	list_iterate_items(seg, &lv->segments) {
		log_print("Logical extent %u to %u:",
			  seg->le, seg->le + seg->len - 1);

		log_print("  Type\t\t%s", seg->segtype->ops->name(seg));

		if (seg->segtype->ops->display)
			seg->segtype->ops->display(seg);
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
               ( s1 = display_size ( LVM_LV_SIZE_MAX(vg), SIZE_SHORT)));
      free ( s1);
*********/
	log_print("Max PV                %u", vg->max_pv);
	log_print("Cur PV                %u", vg->pv_count);
	log_print("Act PV                %u", active_pvs);

	log_print("VG Size               %s",
		  display_size(vg->cmd,
			       (uint64_t) vg->extent_count * vg->extent_size,
			       SIZE_SHORT));

	log_print("PE Size               %s",
		  display_size(vg->cmd, (uint64_t) vg->extent_size,
			       SIZE_SHORT));

	log_print("Total PE              %u", vg->extent_count);

	log_print("Alloc PE / Size       %u / %s",
		  vg->extent_count - vg->free_count,
		  display_size(vg->cmd,
			       ((uint64_t) vg->extent_count - vg->free_count) *
			       vg->extent_size, SIZE_SHORT));

	log_print("Free  PE / Size       %u / %s", vg->free_count,
		  display_size(vg->cmd,
			       (uint64_t) vg->free_count * vg->extent_size,
			       SIZE_SHORT));

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
	uint32_t active_pvs;
	const char *access;
	char uuid[64];

	if (vg->status & PARTIAL_VG)
		active_pvs = list_size(&vg->pvs);
	else
		active_pvs = vg->pv_count;

	switch (vg->status & (LVM_READ | LVM_WRITE)) {
		case LVM_READ | LVM_WRITE:
			access = "r/w";
			break;
		case LVM_READ:
			access = "r";
			break;
		case LVM_WRITE:
			access = "w";
			break;
		default:
			access = "";
	}

	if (!id_write_format(&vg->id, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_print("%s:%s:%d:-1:%u:%u:%u:-1:%u:%u:%u:%" PRIu64 ":%" PRIu32
		  ":%u:%u:%u:%s",
		vg->name,
		access,
		vg->status,
		/* internal volume group number; obsolete */
		vg->max_lv,
		vg->lv_count,
		lvs_in_vg_opened(vg),
		/* FIXME: maximum logical volume size */
		vg->max_pv,
		vg->pv_count,
		active_pvs,
		(uint64_t) vg->extent_count * (vg->extent_size / 2),
		vg->extent_size / 2,
		vg->extent_count,
		vg->extent_count - vg->free_count, 
		vg->free_count,
		uuid[0] ? uuid : "none");
	return;
}

void vgdisplay_short(struct volume_group *vg)
{
	log_print("\"%s\" %-9s [%-9s used / %s free]", vg->name,
/********* FIXME if "open" print "/used" else print "/idle"???  ******/
		  display_size(vg->cmd,
			       (uint64_t) vg->extent_count * vg->extent_size,
			       SIZE_SHORT),
		  display_size(vg->cmd,
			       ((uint64_t) vg->extent_count -
				vg->free_count) * vg->extent_size,
			       SIZE_SHORT),
		  display_size(vg->cmd,
			       (uint64_t) vg->free_count * vg->extent_size,
			       SIZE_SHORT));
	return;
}
