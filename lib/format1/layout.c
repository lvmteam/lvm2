/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "disk-rep.h"
#include "log.h"


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

/*
 * This assumes pe_count and pe_start have already
 * been calculated correctly.
 */
int calculate_layout(struct disk_list *dl)
{
	struct pv_disk *pvd = &dl->pv;

	pvd->pv_on_disk.base = METADATA_BASE;
	pvd->pv_on_disk.size = PV_SIZE;

        pvd->vg_on_disk.base = _next_base(&pvd->pv_on_disk);
        pvd->vg_on_disk.size = VG_SIZE;

        pvd->pv_uuidlist_on_disk.base = _next_base(&pvd->vg_on_disk);
        pvd->pv_uuidlist_on_disk.size = (MAX_PV + 1) * NAME_LEN;

        pvd->lv_on_disk.base = _next_base(&pvd->pv_uuidlist_on_disk);
        pvd->lv_on_disk.size = (MAX_LV + 1) * sizeof(struct lv_disk);

        pvd->pe_on_disk.base = _next_base(&pvd->lv_on_disk);
        pvd->pe_on_disk.size = pvd->pe_total * sizeof(struct pe_disk);

	if (!_adjust_pe_on_disk(pvd)) {
		log_err("insufficient space for metadata and PE's.");
		return 0;
	}

	return 1;
}


