/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "disk-rep.h"
#include "log.h"
#include "dbg_malloc.h"


/*
 * Only works with powers of 2.
 */
static inline ulong _round_up(ulong n, ulong size)
{
	size--;
	return (n + size) & ~size;
}

static inline ulong _div_up(ulong n, ulong size)
{
	return _round_up(n, size) / size;
}

/*
 * Each chunk of metadata should be aligned to
 * METADATA_ALIGN.
 */
static uint32_t _next_base(struct data_area *area)
{
	return _round_up(area->base + area->size, METADATA_ALIGN);
}

/*
 * Quick calculation based on pe_start.
 */
static int _adjust_pe_on_disk(struct pv_disk *pvd)
{
	uint32_t pe_start = pvd->pe_start * SECTOR_SIZE;

	if (pe_start < pvd->pe_on_disk.base + pvd->pe_on_disk.size)
		return 0;

	pvd->pe_on_disk.size = pe_start - pvd->pe_on_disk.base;
	return 1;
}

static void _calc_simple_layout(struct pv_disk *pvd)
{
	pvd->pv_on_disk.base = METADATA_BASE;
	pvd->pv_on_disk.size = PV_SIZE;

        pvd->vg_on_disk.base = _next_base(&pvd->pv_on_disk);
        pvd->vg_on_disk.size = VG_SIZE;

        pvd->pv_uuidlist_on_disk.base = _next_base(&pvd->vg_on_disk);
        pvd->pv_uuidlist_on_disk.size = MAX_PV * NAME_LEN;

        pvd->lv_on_disk.base = _next_base(&pvd->pv_uuidlist_on_disk);
        pvd->lv_on_disk.size = MAX_LV * sizeof(struct lv_disk);

        pvd->pe_on_disk.base = _next_base(&pvd->lv_on_disk);
        pvd->pe_on_disk.size = pvd->pe_total * sizeof(struct pe_disk);
}

int _check_vg_limits(struct disk_list *dl)
{
	if (dl->vg.lv_max >= MAX_LV) {
		log_error("MaxLogicalVolumes of %d exceeds format limit of %d "
			  "for VG '%s'", dl->vg.lv_max, MAX_LV - 1, 
			  dl->pv.vg_name);
		return 0;
	}

	if (dl->vg.pv_max >= MAX_PV) {
		log_error("MaxPhysicalVolumes of %d exceeds format limit of %d "
			  "for VG '%s'", dl->vg.pv_max, MAX_PV - 1, 
			  dl->pv.vg_name);
		return 0;
	}

	return 1;
}

/*
 * This assumes pe_count and pe_start have already
 * been calculated correctly.
 */
int calculate_layout(struct disk_list *dl)
{
	struct pv_disk *pvd = &dl->pv;

	_calc_simple_layout(pvd);
	if (!_adjust_pe_on_disk(pvd)) {
		log_error("Insufficient space for metadata and PE's.");
		return 0;
	}

	if (!_check_vg_limits(dl))
		return 0;

	return 1;
}


/*
 * It may seem strange to have a struct
 * physical_volume in here, but the number of
 * extents that can fit on a disk *is* metadata
 * format dependant.
 */
int calculate_extent_count(struct physical_volume *pv)
{
	struct pv_disk *pvd = dbg_malloc(sizeof(*pvd));
	uint32_t end;

	if (!pvd) {
		stack;
		return 0;
	}

	/*
	 * Guess how many extents will fit,
	 * bearing in mind that one is going to be
	 * knocked off at the start of the next
	 * loop.
	 */
	pvd->pe_total = (pv->size / pv->pe_size);

	if (pvd->pe_total < PE_SIZE_PV_SIZE_REL) {
		log_error("Insufficient space for extents on %s",
			  dev_name(pv->dev));
		return 0;
	}

	do {
		pvd->pe_total--;
		_calc_simple_layout(pvd);
		end = ((pvd->pe_on_disk.base + pvd->pe_on_disk.size) /
		       SECTOR_SIZE);

		pvd->pe_start = _round_up(end, PE_ALIGN);

	} while((pvd->pe_start + (pvd->pe_total * pv->pe_size)) > pv->size);

	pv->pe_count = pvd->pe_total;
	pv->pe_start = pvd->pe_start;
	dbg_free(pvd);
	return 1;
}
