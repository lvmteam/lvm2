/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "disk-rep.h"
#include "pool.h"
#include "xlate.h"
#include "log.h"

#define fail do {stack; return 0;} while(0)
#define xx16(v) disk->v = xlate16(disk->v)
#define xx32(v) disk->v = xlate32(disk->v)
#define xx64(v) disk->v = xlate64(disk->v)

/*
 * Functions to perform the endian conversion
 * between disk and core.  The same code works
 * both ways of course.
 */
static void _xlate_pv(struct pv_disk *disk)
{
        xx16(version);

        xx32(pv_on_disk.base); xx32(pv_on_disk.size);
        xx32(vg_on_disk.base); xx32(vg_on_disk.size);
        xx32(pv_uuidlist_on_disk.base); xx32(pv_uuidlist_on_disk.size);
        xx32(lv_on_disk.base); xx32(lv_on_disk.size);
        xx32(pe_on_disk.base); xx32(pe_on_disk.size);

        xx32(pv_major);
        xx32(pv_number);
        xx32(pv_status);
        xx32(pv_allocatable);
        xx32(pv_size);
        xx32(lv_cur);
        xx32(pe_size);
        xx32(pe_total);
        xx32(pe_allocated);
	xx32(pe_start);
}

static void _xlate_lv(struct lv_disk *disk)
{
        xx32(lv_access);
        xx32(lv_status);
        xx32(lv_open);
        xx32(lv_dev);
        xx32(lv_number);
        xx32(lv_mirror_copies);
        xx32(lv_recovery);
        xx32(lv_schedule);
        xx32(lv_size);
        xx32(lv_snapshot_minor);
        xx16(lv_chunk_size);
        xx16(dummy);
        xx32(lv_allocated_le);
        xx32(lv_stripes);
        xx32(lv_stripesize);
        xx32(lv_badblock);
        xx32(lv_allocation);
        xx32(lv_io_timeout);
        xx32(lv_read_ahead);
}

static void _xlate_vg(struct vg_disk *disk)
{
        xx32(vg_number);
        xx32(vg_access);
        xx32(vg_status);
        xx32(lv_max);
        xx32(lv_cur);
        xx32(lv_open);
        xx32(pv_max);
        xx32(pv_cur);
        xx32(pv_act);
        xx32(dummy);
        xx32(vgda);
        xx32(pe_size);
        xx32(pe_total);
        xx32(pe_allocated);
        xx32(pvg_total);
}

static void _xlate_extents(struct pe_disk *extents, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		extents[i].lv_num = xlate16(extents[i].lv_num);
		extents[i].le_num = xlate16(extents[i].le_num);
	}
}

/*
 * Handle both minor metadata formats.
 */
static int _munge_formats(struct pv_disk *pvd)
{
	uint32_t pe_start;

	switch (pvd->version) {
	case 1:
		pe_start = (pvd->pe_on_disk.base + pvd->pe_on_disk.size) / 
			SECTOR_SIZE;
		break;

	case 2:
		pvd->version = 1;
		pe_start = pvd->pe_start * SECTOR_SIZE;
		pvd->pe_on_disk.size = pe_start - pvd->pe_on_disk.base;
		break;

	default:
		return 0;
	}

	return 1;
}

static int _read_pv(struct disk_list *data)
{
	struct pv_disk *pvd = &data->pv;
	if (dev_read(data->dev, 0, sizeof(*pvd), pvd) != sizeof(*pvd))
		fail;
	_xlate_pv(pvd);

	return 1;
}

static int _read_lv(struct device *dev, ulong pos, struct lv_disk *disk)
{
	if (dev_read(dev, pos, sizeof(*disk), disk) != sizeof(*disk))
		fail;

	_xlate_lv(disk);

	return 1;
}

static int _read_vg(struct disk_list *data)
{
	struct vg_disk *vgd = &data->vg;
	unsigned long pos = data->pv.vg_on_disk.base;
	if (dev_read(data->dev, pos, sizeof(*vgd), vgd) != sizeof(*vgd))
		fail;

	_xlate_vg(vgd);

	return 1;
}

static int _read_uuids(struct disk_list *data)
{
	int num_read = 0;
	struct uuid_list *ul;
	char buffer[NAME_LEN];
	ulong pos = data->pv.pv_uuidlist_on_disk.base;
	ulong end = pos + data->pv.pv_uuidlist_on_disk.size;

	while(pos < end && num_read < data->vg.pv_cur) {
		if (dev_read(data->dev, pos, sizeof(buffer), buffer) !=
		    sizeof(buffer))
			fail;

		if (!(ul = pool_alloc(data->mem, sizeof(*ul))))
			fail;

		memcpy(ul->uuid, buffer, NAME_LEN);
		ul->uuid[NAME_LEN] = '\0';

		list_add(&ul->list, &data->uuids);

		pos += NAME_LEN;
		num_read++;
	}

	return 1;
}

static int _read_lvs(struct disk_list *data)
{
	int i;
	unsigned long pos;
	struct lvd_list *ll;

	for(i = 0; i < data->vg.lv_cur; i++) {
		pos = data->pv.lv_on_disk.base + (i * sizeof(struct lv_disk));
		ll = pool_alloc(data->mem, sizeof(*ll));

		if (!ll)
			fail;

		if (!_read_lv(data->dev, pos, &ll->lv))
			fail;

		list_add(&ll->list, &data->lvs);
	}

	return 1;
}

static int _read_extents(struct disk_list *data)
{
	size_t len = sizeof(struct pe_disk) * data->pv.pe_total;
	struct pe_disk *extents = pool_alloc(data->mem, len);
	unsigned long pos = data->pv.pe_on_disk.base;

	if (!extents)
		fail;

	if (dev_read(data->dev, pos, len, extents) != len)
		fail;

	_xlate_extents(extents, data->pv.pe_total);
	data->extents = extents;

	return 1;
}

struct disk_list *read_pv(struct device *dev, struct pool *mem,
			  const char *vg_name)
{
	struct disk_list *data = pool_alloc(mem, sizeof(*data));
	data->dev = dev;
	data->mem = mem;
	INIT_LIST_HEAD(&data->uuids);
	INIT_LIST_HEAD(&data->lvs);

	if (!_read_pv(data)) {
		log_debug("Failed to read PV data from %s", dev->name);
		goto bad;
	}

	if (data->pv.id[0] != 'H' || data->pv.id[1] != 'M') {
		log_debug("%s does not have a valid PV identifier.",
			 dev->name);
		goto bad;
	}

	if (!_munge_formats(&data->pv)) {
		log_verbose("Unknown metadata version %d found on %s", 
			    data->pv.version, dev->name);
		goto bad;
	}

	/*
	 * is it an orphan ?
	 */
	if (data->pv.vg_name == '\0') {
		log_very_verbose("%s is not a member of any VG",
				 dev->name);
		return data;
	}

	if (vg_name && strcmp(vg_name, data->pv.vg_name)) {
		log_very_verbose("%s is not a member of the VG %s",
			 dev->name, vg_name);
		goto bad;
	}

	if (!_read_vg(data)) {
		log_error("Failed to read VG data from PV (%s)", dev->name);
		goto bad;
	}

	if (!_read_uuids(data)) {
		log_error("Failed to read PV uuid list from %s", dev->name);
		goto bad;
	}

	if (!_read_lvs(data)) {
		log_error("Failed to read LV's from %s", dev->name);
		goto bad;
	}

	if (!_read_extents(data)) {
		log_error("Failed to read extents from %s", dev->name);
		goto bad;
	}

	return data;

 bad:
	pool_free(data->mem, data);
	return NULL;
}

/*
 * Build a list of pv_d's structures, allocated
 * from mem.  We keep track of the first object
 * allocated form the pool so we can free off all
 * the memory if something goes wrong.
 */
int read_pvs_in_vg(const char *vg_name, struct dev_filter *filter,
		   struct pool *mem, struct list_head *head)
{
	struct dev_iter *iter = dev_iter_create(filter);
	struct device *dev;
	struct disk_list *data = NULL;

	for (dev = dev_iter_get(iter); dev; dev = dev_iter_get(iter)) {
		if ((data = read_pv(dev, mem, vg_name)))
			list_add(&data->list, head);
	}
	dev_iter_destroy(iter);

	return 1;
}


static int _write_vg(struct disk_list *data)
{
	struct vg_disk *vgd = &data->vg;
	unsigned long pos = data->pv.vg_on_disk.base;

	_xlate_vg(vgd);
	if (dev_write(data->dev, pos, sizeof(*vgd), vgd) != sizeof(*vgd))
		fail;

	_xlate_vg(vgd);

	return 1;
}

static int _write_uuids(struct disk_list *data)
{
	struct uuid_list *ul;
	struct list_head *tmp;
	ulong pos = data->pv.pv_uuidlist_on_disk.base;
	ulong end = pos + data->pv.pv_uuidlist_on_disk.size;

	list_for_each(tmp, &data->uuids) {
		if (pos >= end) {
			log_error("Too many uuids to fit on %s",
				data->dev->name);
			return 0;
		}

		ul = list_entry(tmp, struct uuid_list, list);
		if (dev_write(data->dev, pos,
			      sizeof(NAME_LEN), ul->uuid) != NAME_LEN)
			fail;

		pos += NAME_LEN;
	}

	return 1;
}

static int _write_lv(struct device *dev, ulong pos, struct lv_disk *disk)
{
	_xlate_lv(disk);
	if (dev_write(dev, pos, sizeof(*disk), disk) != sizeof(*disk))
		fail;

	_xlate_lv(disk);

	return 1;
}

static int _write_lvs(struct disk_list *data)
{
	struct list_head *tmp;
	unsigned long pos;

	list_for_each(tmp, &data->lvs) {
		struct lvd_list *ll = list_entry(tmp, struct lvd_list, list);
		pos = data->pv.lv_on_disk.base;

		if (!_write_lv(data->dev, pos, &ll->lv))
			fail;

		pos += sizeof(struct lv_disk);
	}

	return 1;
}

static int _write_extents(struct disk_list *data)
{
	size_t len = sizeof(struct pe_disk) * data->pv.pe_total;
	struct pe_disk *extents = data->extents;

	_xlate_extents(extents, data->pv.pe_total);
	if (dev_write(data->dev, 0, len, extents) != len)
		fail;

	_xlate_extents(extents, data->pv.pe_total);

	return 1;
}

static int _write_pv(struct disk_list *data)
{
	struct pv_disk *disk = &data->pv;

	_xlate_pv(disk);
	if (dev_write(data->dev, 0, sizeof(*disk), disk) != sizeof(*disk))
		fail;

	_xlate_pv(disk);

	return 1;
}

static int _write_all_pv(struct disk_list *data)
{
	const char *pv_name = data->dev->name;

	if (!_write_pv(data)) {
		log_error("Failed to write PV structure onto %s", pv_name);
		return 0;
	}

	/*
	 * Stop here for orphan pv's.
	 */
	if (data->pv.vg_name[0] == '\0')
		return 1;

	if (!_write_vg(data)) {
		log_error("Failed to write VG data to %s", pv_name);
		return 0;
	}

	if (!_write_uuids(data)) {
		log_error("Failed to write PV uuid list to %s", pv_name);
		return 0;
	}

	if (!_write_lvs(data)) {
		log_error("Failed to write LV's to %s", pv_name);
		return 0;
	}

	if (!_write_extents(data)) {
		log_error("Failed to write extents to %s", pv_name);
		return 0;
	}

	return 1;
}

/*
 * Writes all the given pv's to disk.  Does very
 * little sanity checking, so make sure correct
 * data is passed to here.
 */
int write_pvs(struct list_head *pvs)
{
	struct list_head *tmp;
	struct disk_list *dl;

	list_for_each(tmp, pvs) {
		dl = list_entry(tmp, struct disk_list, list);
		if (!(_write_all_pv(dl)))
			fail;

		log_debug("Successfully wrote data to %s", dl->dev->name);
	}

	return 1;
}
