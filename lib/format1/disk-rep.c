/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 *
 */

#include "disk-rep.h"
#include "pool.h"

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
        xx32(pv_uuid_on_disk.base); xx32(pv_uuid_on_disk.size);
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

	/* FIXME: put v1, v2 munging in here. */
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

static int _read_pv(struct device *dev, struct pv_disk *disk)
{
	if (dev_read(dev, 0, sizeof(*disk), disk) != sizeof(&disk)) {
		log_error("failed to read pv from disk (%s)", dev->name);
		return 0;
	}

	_xlate_pv(disk);
	memset(disk->pv_name, 0, sizeof (disk->pv_name));
	strncpy(disk->pv_name, dev->name, sizeof(disk->pv_name) - 1);

	disk->pv_dev = dev->dev;
	return 1;
}

static int _read_lv(struct device *dev, ulong pos, struct lv_disk *disk)
{
	if (dev_read(dev, pos, sizeof(*disk), disk) != sizeof(*disk)) {
		log_error("failed to read lv from disk (%s)", dev->name);
		return 0;
	}

	_xlate_lv(disk);
	return 1;
}

static int _read_vg(struct device *dev, ulong pos, struct vg_disk *disk)
{
	if (dev_read(dev, pos, sizeof(*disk), disk) != sizeof(*disk)) {
		log_error("failed to read vg from disk (%s)", dev->name);
		return 0;
	}

	_xlate_vg(disk);
	return 1;
}

static int _read_uuids(struct device *dev, struct disk_list *data)
{
	int num_read = 0;
	struct uuid_list *ul;
	char buffer[NAME_LEN];
	ulong pos = data->pv.pv_uuidlist_on_disk.base;
	ulong end = pos + data->pv.pv_uuidlist_on_disk.size;

	while(pos < end && num_read < ABS_MAX_PV) {
		if (dev_read(dev, pos, sizeof(buffer), buffer) !=
		    sizeof(buffer)) {
			log_err("failed to read a pv_uuid from %s\n",
				dev->name);
			return 0;
		}

		if (!(ul = pool_alloc(data->mem, sizeof(*ul)))) {
			stack;
			return 0;
		}

		memcpy(ul->uuid, buffer, NAME_LEN);
		ul->uuid[NAME_LEN] = '\0';

		list_add(&ui->list, &data->uuids);

		pos += NAME_LEN;
		num_read++;
	}
	return 1;
}

static int _read_lvs(struct device *dev, struct disk_list *data)
{
	int i;
	unsigned long pos;

	for(i = 0; i < data->vg.lv_cur; i++) {
		pos = data->pv.lv_on_disk.base + (i * sizeof(struct lv_list));
		struct lv_list *ll = pool_alloc(sizeof(*ll));

		if (!ll) {
			stack;
			return 0;
		}

		if (!_read_lv(dev, pos, &ll->lv)) {
			stack;
			return 0;
		}

		list_add(&ll->list, &data->lvs);
	}

	return 1;
}

static int _read_extents(struct device *dev, struct disk_list *data)
{
	size_t len = sizeof(struct pe_disk) * data->pv->pe_total;
	struct pe_disk *extents = pool_alloc(data->mem, len);
	unsigned long pos = data->pv.pe_on_disk.base;

	if (!extents) {
		stack;
		return 0;
	}

	if (dev_read(dev, pos, len, extents) != len) {
		log_error("failed to read extents from disk (%s)", dev->name);
		return 0;
	}

	for (i = 0; i < data->pv->pe_total; i++) {
		extents[i].lv_num = xlate16(extents[i].lv_num);
		extents[i].le_num = xlate16(extents[i].le_num);
	}

	data->extents = extents;
	return 1;
}

static int _read_all_pv_data(struct device *dev, struct disk_list *data)
{
	if (!_read_pv(dev, &data->pv)) {
		stack;
		return 0;
	}

	if (strcmp(data->pv.id, "HM")) {
		log_info("%s does not have a valid PV identifier.\n",
			 dev->name);
		return 0;
	}

	if (!_read_vg(dev, data->pv.pv_vg_on_disk.base, &data->vg)) {
		log_err("failed to read vg data from pv (%s)\n", dev->name);
		return 0;
	}

	if (!_read_uuids(dev, data)) {
		log_err("failed to read pv uuid list from %s\n", dev->name);
		return 0;
	}

	if (!_read_lvs(dev, data)) {
		log_err("failed to read lv's from %s\n", dev->name);
		return 0;
	}

	if (!_read_extents(dev, data)) {
		log_err("failed to read extents from %s\n", dev->name);
		return 0;
	}

	return 1;
}

/*
 * Build a list of pv_d's structures, allocated
 * from mem.  We keep track of the first object
 * allocated form the pool so we can free off all
 * the memory if something goes wrong.
 */
int read_pvs_in_vg(struct v1 *v, const char *vg_name,
		   struct pool *mem, struct list_head *head)
{
	struct dev_cache_iter *iter = dev_iter_create(v->filter);
	struct device *dev;
	struct disk_list *data = NULL;

	for (dev = dev_iter_get(iter); dev; dev = dev_iter_get(iter)) {
		struct disk_list *data = pool_alloc(mem, sizeof(*data));

		if (!data) {
			stack;
			goto bad;
		}

		if (!first)
			first = data;

		if (_read_all_pv_data(dev, pvd) &&
		    !strcmp(pvd->pv.vg_name, vg_name))
			list_add(&pvd->list, head);
		else
			pool_free(mem, pvd);
	}
	return 1;

 bad:
	if (first)
		pool_free(mem, first);
	return 0;
}
