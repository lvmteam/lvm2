/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "disk-rep.h"
#include "pool.h"
#include "xlate.h"
#include "log.h"
#include "vgcache.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define fail do {stack; return 0;} while(0)
#define xx16(v) disk->v = xlate16(disk->v)
#define xx32(v) disk->v = xlate32(disk->v)
#define xx64(v) disk->v = xlate64(disk->v)

/*
 * Functions to perform the endian conversion
 * between disk and core.  The same code works
 * both ways of course.
 */
static void _xlate_pvd(struct pv_disk *disk)
{
	xx16(version);

	xx32(pv_on_disk.base);
	xx32(pv_on_disk.size);
	xx32(vg_on_disk.base);
	xx32(vg_on_disk.size);
	xx32(pv_uuidlist_on_disk.base);
	xx32(pv_uuidlist_on_disk.size);
	xx32(lv_on_disk.base);
	xx32(lv_on_disk.size);
	xx32(pe_on_disk.base);
	xx32(pe_on_disk.size);

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

static void _xlate_lvd(struct lv_disk *disk)
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

static void _xlate_vgd(struct vg_disk *disk)
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
		pvd->pe_start = ((pvd->pe_on_disk.base +
				  pvd->pe_on_disk.size) / SECTOR_SIZE);
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

static int _read_pvd(struct disk_list *data)
{
	struct pv_disk *pvd = &data->pvd;
	if (dev_read(data->dev, 0, sizeof(*pvd), pvd) != sizeof(*pvd))
		fail;
	_xlate_pvd(pvd);

	return 1;
}

static int _read_lvd(struct device *dev, ulong pos, struct lv_disk *disk)
{
	if (dev_read(dev, pos, sizeof(*disk), disk) != sizeof(*disk))
		fail;

	_xlate_lvd(disk);

	return 1;
}

static int _read_vgd(struct disk_list *data)
{
	struct vg_disk *vgd = &data->vgd;
	unsigned long pos = data->pvd.vg_on_disk.base;
	if (dev_read(data->dev, pos, sizeof(*vgd), vgd) != sizeof(*vgd))
		fail;

	_xlate_vgd(vgd);

	return 1;
}

static int _read_uuids(struct disk_list *data)
{
	int num_read = 0;
	struct uuid_list *ul;
	char buffer[NAME_LEN];
	ulong pos = data->pvd.pv_uuidlist_on_disk.base;
	ulong end = pos + data->pvd.pv_uuidlist_on_disk.size;

	while (pos < end && num_read < data->vgd.pv_cur) {
		if (dev_read(data->dev, pos, sizeof(buffer), buffer) !=
		    sizeof(buffer))
			fail;

		if (!(ul = pool_alloc(data->mem, sizeof(*ul))))
			fail;

		memcpy(ul->uuid, buffer, NAME_LEN);
		ul->uuid[NAME_LEN - 1] = '\0';

		list_add(&data->uuids, &ul->list);

		pos += NAME_LEN;
		num_read++;
	}

	return 1;
}

static inline int _check_lvd(struct lv_disk *lvd)
{
	return !(lvd->lv_name[0] == '\0');
}

static int _read_lvs(struct disk_list *data)
{
	int i, read = 0;
	unsigned long pos;
	struct lvd_list *ll;
	struct vg_disk *vgd = &data->vgd;

	for (i = 0; (i < vgd->lv_max) && (read < vgd->lv_cur); i++) {
		pos = data->pvd.lv_on_disk.base + (i * sizeof(struct lv_disk));
		ll = pool_alloc(data->mem, sizeof(*ll));

		if (!ll)
			fail;

		if (!_read_lvd(data->dev, pos, &ll->lvd))
			fail;

		if (!_check_lvd(&ll->lvd))
			continue;

		read++;
		list_add(&data->lvds, &ll->list);
	}

	return 1;
}

static int _read_extents(struct disk_list *data)
{
	size_t len = sizeof(struct pe_disk) * data->pvd.pe_total;
	struct pe_disk *extents = pool_alloc(data->mem, len);
	unsigned long pos = data->pvd.pe_on_disk.base;

	if (!extents)
		fail;

	if (dev_read(data->dev, pos, len, extents) != len)
		fail;

	_xlate_extents(extents, data->pvd.pe_total);
	data->extents = extents;

	return 1;
}

static struct disk_list *__read_disk(struct device *dev, struct pool *mem,
				     const char *vg_name)
{
	struct disk_list *data = pool_alloc(mem, sizeof(*data));
	const char *name = dev_name(dev);

	if (!data) {
		stack;
		return NULL;
	}

	data->dev = dev;
	data->mem = mem;
	list_init(&data->uuids);
	list_init(&data->lvds);

	if (!_read_pvd(data)) {
		log_debug("Failed to read PV data from %s", name);
		goto bad;
	}

	if (data->pvd.id[0] != 'H' || data->pvd.id[1] != 'M') {
		log_very_verbose("%s does not have a valid PV identifier",
				 name);
		goto bad;
	}

	if (!_munge_formats(&data->pvd)) {
		log_very_verbose("Unknown metadata version %d found on %s",
				 data->pvd.version, name);
		goto bad;
	}

	/* Update VG cache with whatever we found */
	vgcache_add(data->pvd.vg_name, dev);

	/*
	 * is it an orphan ?
	 */
	if (data->pvd.vg_name == '\0') {
		log_very_verbose("%s is not a member of any VG", name);
		return data;
	}

	if (vg_name && strcmp(vg_name, data->pvd.vg_name)) {
		log_very_verbose("%s is not a member of the VG %s",
				 name, vg_name);
		goto bad;
	}

	if (!_read_vgd(data)) {
		log_error("Failed to read VG data from PV (%s)", name);
		goto bad;
	}

	if (!_read_uuids(data)) {
		log_error("Failed to read PV uuid list from %s", name);
		goto bad;
	}

	if (!_read_lvs(data)) {
		log_error("Failed to read LV's from %s", name);
		goto bad;
	}

	if (!_read_extents(data)) {
		log_error("Failed to read extents from %s", name);
		goto bad;
	}

	log_very_verbose("Found %s in VG %s", name, data->pvd.vg_name);

	return data;

      bad:
	pool_free(data->mem, data);
	return NULL;
}

struct disk_list *read_disk(struct device *dev, struct pool *mem,
			    const char *vg_name)
{
	struct disk_list *r;

	if (!dev_open(dev, O_RDONLY)) {
		stack;
		return NULL;
	}

	r = __read_disk(dev, mem, vg_name);

	if (!dev_close(dev))
		stack;

	return r;
}

/*
 * Build a list of pv_d's structures, allocated from mem.  
 * We keep track of the first object allocated form the pool
 * so we can free off all the memory if something goes wrong.
 */
int read_pvs_in_vg(const char *vg_name, struct dev_filter *filter,
		   struct pool *mem, struct list *head)
{
	struct dev_iter *iter;
	struct device *dev;
	struct disk_list *data = NULL;

	struct list *pvdh, *pvdh2;

	/* Fast path if we already saw this VG and cached the list of PVs */
	if ((pvdh = vgcache_find(vg_name))) {
		list_iterate(pvdh2, pvdh) {
			dev = list_item(pvdh2, struct pvdev_list)->dev;
			if ((data = read_disk(dev, mem, vg_name)))
				list_add(head, &data->list);
		}

		/* Did we find the whole VG? */
		if (!vg_name || !*vg_name ||
		    (data && *data->pvd.vg_name &&
		     list_size(head) == data->vgd.pv_cur))
			return 1;

		/* Something changed. Remove the hints. */
		list_init(head);
		vgcache_del(vg_name);
	}

	if (!(iter = dev_iter_create(filter))) {
		log_error("read_pvs_in_vg: dev_iter_create failed");
		return 0;
	}

	/* Otherwise do a complete scan */
	for (dev = dev_iter_get(iter); dev; dev = dev_iter_get(iter)) {
		if ((data = read_disk(dev, mem, vg_name))) {
			list_add(head, &data->list);
		}
	}
	dev_iter_destroy(iter);

	if (list_empty(head))
		return 0;

	return 1;
}

static int _write_vgd(struct disk_list *data)
{
	struct vg_disk *vgd = &data->vgd;
	unsigned long pos = data->pvd.vg_on_disk.base;

	_xlate_vgd(vgd);
	if (dev_write(data->dev, pos, sizeof(*vgd), vgd) != sizeof(*vgd))
		fail;

	_xlate_vgd(vgd);

	return 1;
}

static int _write_uuids(struct disk_list *data)
{
	struct uuid_list *ul;
	struct list *uh;
	ulong pos = data->pvd.pv_uuidlist_on_disk.base;
	ulong end = pos + data->pvd.pv_uuidlist_on_disk.size;

	list_iterate(uh, &data->uuids) {
		if (pos >= end) {
			log_error("Too many uuids to fit on %s",
				  dev_name(data->dev));
			return 0;
		}

		ul = list_item(uh, struct uuid_list);
		if (dev_write(data->dev, pos, NAME_LEN, ul->uuid) != NAME_LEN)
			fail;

		pos += NAME_LEN;
	}

	return 1;
}

static int _write_lvd(struct device *dev, ulong pos, struct lv_disk *disk)
{
	_xlate_lvd(disk);
	if (dev_write(dev, pos, sizeof(*disk), disk) != sizeof(*disk))
		fail;

	_xlate_lvd(disk);

	return 1;
}

static int _write_lvs(struct disk_list *data)
{
	struct list *lvh;
	unsigned long pos;

	pos = data->pvd.lv_on_disk.base;

	if (!dev_zero(data->dev, pos, data->pvd.lv_on_disk.size)) {
		log_error("Couldn't zero lv area on device '%s'",
			  dev_name(data->dev));
		return 0;
	}

	list_iterate(lvh, &data->lvds) {
		struct lvd_list *ll = list_item(lvh, struct lvd_list);

		if (!_write_lvd(data->dev, pos, &ll->lvd))
			fail;

		pos += sizeof(struct lv_disk);
	}

	return 1;
}

static int _write_extents(struct disk_list *data)
{
	size_t len = sizeof(struct pe_disk) * data->pvd.pe_total;
	struct pe_disk *extents = data->extents;
	unsigned long pos = data->pvd.pe_on_disk.base;

	_xlate_extents(extents, data->pvd.pe_total);
	if (dev_write(data->dev, pos, len, extents) != len)
		fail;

	_xlate_extents(extents, data->pvd.pe_total);

	return 1;
}

static int _write_pvd(struct disk_list *data)
{
	struct pv_disk *disk = &data->pvd;

	_xlate_pvd(disk);
	if (dev_write(data->dev, 0, sizeof(*disk), disk) != sizeof(*disk))
		fail;

	_xlate_pvd(disk);

	return 1;
}

/*
 * assumes the device has been opened.
 */
static int __write_all_pvd(struct disk_list *data)
{
	const char *pv_name = dev_name(data->dev);

	if (!_write_pvd(data)) {
		log_error("Failed to write PV structure onto %s", pv_name);
		return 0;
	}

	vgcache_add(data->pvd.vg_name, data->dev);
	/*
	 * Stop here for orphan pv's.
	 */
	if (data->pvd.vg_name[0] == '\0')
		return 1;

	if (!_write_vgd(data)) {
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
 * opens the device and hands to the above fn.
 */
static int _write_all_pvd(struct disk_list *data)
{
	int r;

	if (!dev_open(data->dev, O_WRONLY)) {
		stack;
		return 0;
	}

	r = __write_all_pvd(data);

	if (!dev_close(data->dev))
		stack;

	return r;
}

/*
 * Writes all the given pv's to disk.  Does very
 * little sanity checking, so make sure correct
 * data is passed to here.
 */
int write_disks(struct list *pvs)
{
	struct list *pvh;
	struct disk_list *dl;

	list_iterate(pvh, pvs) {
		dl = list_item(pvh, struct disk_list);
		if (!(_write_all_pvd(dl)))
			fail;

		log_debug("Successfully wrote data to %s", dev_name(dl->dev));
	}

	return 1;
}
