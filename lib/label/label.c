/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2018 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib.h"
#include "label.h"
#include "crc.h"
#include "xlate.h"
#include "lvmcache.h"
#include "lvmetad.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* FIXME Allow for larger labels?  Restricted to single sector currently */

static struct dm_pool *_labeller_mem;

/*
 * Internal labeller struct.
 */
struct labeller_i {
	struct dm_list list;

	struct labeller *l;
	char name[0];
};

static struct dm_list _labellers;

static struct labeller_i *_alloc_li(const char *name, struct labeller *l)
{
	struct labeller_i *li;
	size_t len;

	len = sizeof(*li) + strlen(name) + 1;

	if (!(li = dm_malloc(len))) {
		log_error("Couldn't allocate memory for labeller list object.");
		return NULL;
	}

	li->l = l;
	strcpy(li->name, name);

	return li;
}

int label_init(void)
{
	if (!(_labeller_mem = dm_pool_create("label scan", 128))) {
		log_error("Labeller pool creation failed.");
		return 0;
	}

	dm_list_init(&_labellers);

	return 1;
}

void label_exit(void)
{
	struct labeller_i *li, *tli;

	dm_list_iterate_items_safe(li, tli, &_labellers) {
		dm_list_del(&li->list);
		li->l->ops->destroy(li->l);
		dm_free(li);
	}

	dm_list_init(&_labellers);

	dm_pool_destroy(_labeller_mem);
}

int label_register_handler(struct labeller *handler)
{
	struct labeller_i *li;

	if (!(li = _alloc_li(handler->fmt->name, handler)))
		return_0;

	dm_list_add(&_labellers, &li->list);
	return 1;
}

struct labeller *label_get_handler(const char *name)
{
	struct labeller_i *li;

	dm_list_iterate_items(li, &_labellers)
		if (!strcmp(li->name, name))
			return li->l;

	return NULL;
}

static void _update_lvmcache_orphan(struct lvmcache_info *info)
{
	struct lvmcache_vgsummary vgsummary_orphan = {
		.vgname = lvmcache_fmt(info)->orphan_vg_name,
	};

        memcpy(&vgsummary_orphan.vgid, lvmcache_fmt(info)->orphan_vg_name, strlen(lvmcache_fmt(info)->orphan_vg_name));

	if (!lvmcache_update_vgname_and_id(info, &vgsummary_orphan))
		stack;
}

struct find_labeller_params {
	struct device *dev;
	uint64_t scan_sector;	/* Sector to be scanned */
	uint64_t label_sector;	/* Sector where label found */
	lvm_callback_fn_t process_label_data_fn;
	void *process_label_data_context;

	struct label **result;

	int ret;
};

static void _set_label_read_result(int failed, void *context, void *data)
{
	struct find_labeller_params *flp = context;
	struct label **result = flp->result;

	if (failed) {
		flp->ret = 0;
		goto_out;
	}

	if (result && *result) {
		(*result)->dev = flp->dev;
		(*result)->sector = flp->label_sector;
	}

out:
	if (!dev_close(flp->dev))
		stack;

	if (flp->ret && flp->process_label_data_fn)
		flp->process_label_data_fn(0, flp->process_label_data_context, NULL);
}

static void _find_labeller(int failed, void *context, void *data)
{
	struct find_labeller_params *flp = context;
	char *readbuf = data;
	struct device *dev = flp->dev;
	uint64_t scan_sector = flp->scan_sector;
	struct label **result = flp->result;
	char labelbuf[LABEL_SIZE] __attribute__((aligned(8)));
	struct labeller_i *li;
	struct labeller *l = NULL;	/* Set when a labeller claims the label */
	struct label_header *lh;
	struct lvmcache_info *info;
	uint64_t sector;

	if (failed) {
		log_debug_devs("%s: Failed to read label area", dev_name(dev));
		_set_label_read_result(1, flp, NULL);
		return;
	}

	/* Scan a few sectors for a valid label */
	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {
		lh = (struct label_header *) (readbuf + (sector << SECTOR_SHIFT));

		if (!strncmp((char *)lh->id, LABEL_ID, sizeof(lh->id))) {
			if (l) {
				log_error("Ignoring additional label on %s at "
					  "sector %" PRIu64, dev_name(dev),
					  sector + scan_sector);
			}
			if (xlate64(lh->sector_xl) != sector + scan_sector) {
				log_very_verbose("%s: Label for sector %" PRIu64
						 " found at sector %" PRIu64
						 " - ignoring", dev_name(dev),
						 (uint64_t)xlate64(lh->sector_xl),
						 sector + scan_sector);
				continue;
			}
			if (calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl, LABEL_SIZE -
				     ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)) !=
			    xlate32(lh->crc_xl)) {
				log_very_verbose("Label checksum incorrect on %s - "
						 "ignoring", dev_name(dev));
				continue;
			}
			if (l)
				continue;
		}

		dm_list_iterate_items(li, &_labellers) {
			if (li->l->ops->can_handle(li->l, (char *) lh,
						   sector + scan_sector)) {
				log_very_verbose("%s: %s label detected at "
					         "sector %" PRIu64, 
						 dev_name(dev), li->name,
						 sector + scan_sector);
				if (l) {
					log_error("Ignoring additional label "
						  "on %s at sector %" PRIu64,
						  dev_name(dev),
						  sector + scan_sector);
					continue;
				}
				memcpy(labelbuf, lh, LABEL_SIZE);
				flp->label_sector = sector + scan_sector;
				l = li->l;
				break;
			}
		}
	}

	dm_free(readbuf);

	if (!l) {
		if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 0)))
			_update_lvmcache_orphan(info);
		log_very_verbose("%s: No label detected", dev_name(dev));
		flp->ret = 0;
		_set_label_read_result(1, flp, NULL);
	} else
		(void) (l->ops->read)(l, dev, labelbuf, result, &_set_label_read_result, flp);
}

/* FIXME Also wipe associated metadata area headers? */
int label_remove(struct device *dev)
{
	char labelbuf[LABEL_SIZE] __attribute__((aligned(8)));
	int r = 1;
	uint64_t sector;
	int wipe;
	struct labeller_i *li;
	struct label_header *lh;
	struct lvmcache_info *info;
	char *buf = NULL;

	memset(labelbuf, 0, LABEL_SIZE);

	log_very_verbose("Scanning for labels to wipe from %s", dev_name(dev));

	if (!dev_open(dev))
		return_0;

	/*
	 * We flush the device just in case someone is stupid
	 * enough to be trying to import an open pv into lvm.
	 */
	dev_flush(dev);

	if (!(buf = dev_read(dev, UINT64_C(0), LABEL_SCAN_SIZE, DEV_IO_LABEL))) {
		log_debug_devs("%s: Failed to read label area", dev_name(dev));
		goto out;
	}

	/* Scan first few sectors for anything looking like a label */
	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {
		lh = (struct label_header *) (buf + (sector << SECTOR_SHIFT));

		wipe = 0;

		if (!strncmp((char *)lh->id, LABEL_ID, sizeof(lh->id))) {
			if (xlate64(lh->sector_xl) == sector)
				wipe = 1;
		} else {
			dm_list_iterate_items(li, &_labellers) {
				if (li->l->ops->can_handle(li->l, (char *) lh,
							   sector)) {
					wipe = 1;
					break;
				}
			}
		}

		if (wipe) {
			log_very_verbose("%s: Wiping label at sector %" PRIu64,
					 dev_name(dev), sector);
			if (dev_write(dev, sector << SECTOR_SHIFT, LABEL_SIZE, DEV_IO_LABEL, labelbuf)) {
				/* Also remove the PV record from cache. */
				info = lvmcache_info_from_pvid(dev->pvid, dev, 0);
				if (info)
					lvmcache_del(info);
			} else {
				log_error("Failed to remove label from %s at "
					  "sector %" PRIu64, dev_name(dev),
					  sector);
				r = 0;
			}
		}
	}

      out:
	if (!dev_close(dev))
		stack;

	dm_free(buf);
	return r;
}

static int _label_read(struct device *dev, uint64_t scan_sector, struct label **result,
		       lvm_callback_fn_t process_label_data_fn, void *process_label_data_context)
{
	struct lvmcache_info *info;
	struct find_labeller_params *flp;

	if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 1))) {
		log_debug_devs("Reading label from lvmcache for %s", dev_name(dev));
		*result = lvmcache_get_label(info);
		if (process_label_data_fn)
			process_label_data_fn(0, process_label_data_context, NULL);
		return 1;
	}

	if (!(flp = dm_pool_zalloc(_labeller_mem, sizeof *flp))) {
		log_error("find_labeller_params allocation failed.");
		return 0;
	}

	flp->dev = dev;
	flp->scan_sector = scan_sector;
	flp->result = result;
	flp->process_label_data_fn = process_label_data_fn;
	flp->process_label_data_context = process_label_data_context;
	flp->ret = 1;

	/* Ensure result is always wiped as a precaution */
	if (result)
		*result = NULL;

	log_debug_devs("Reading label from device %s", dev_name(dev));

	if (!dev_open_readonly(dev)) {
		stack;

		if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 0)))
			_update_lvmcache_orphan(info);

		return 0;
	}

	if (!(dev_read_callback(dev, scan_sector << SECTOR_SHIFT, LABEL_SCAN_SIZE, DEV_IO_LABEL, _find_labeller, flp))) {
		log_debug_devs("%s: Failed to read label area", dev_name(dev));
		_set_label_read_result(1, flp, NULL);
		return 0;
	}

	return flp->ret;
}

int label_read(struct device *dev, struct label **result, uint64_t scan_sector)
{
	return _label_read(dev, scan_sector, result, NULL, NULL);
}

int label_read_callback(struct dm_pool *mem, struct device *dev, uint64_t scan_sector,
		       lvm_callback_fn_t process_label_data_fn, void *process_label_data_context)
{
	struct label **result;	/* FIXME Eliminate this */

	if (!(result = dm_pool_zalloc(mem, sizeof(*result)))) {
		log_error("Couldn't allocate memory for internal result pointer.");
		return 0;
	}

	return _label_read(dev, scan_sector, result, process_label_data_fn, process_label_data_context);
}

/* Caller may need to use label_get_handler to create label struct! */
int label_write(struct device *dev, struct label *label)
{
	char buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label_header *lh = (struct label_header *) buf;
	int r = 1;

	if (!label->labeller->ops->write) {
		log_error("Label handler does not support label writes");
		return 0;
	}

	if ((LABEL_SIZE + (label->sector << SECTOR_SHIFT)) > LABEL_SCAN_SIZE) {
		log_error("Label sector %" PRIu64 " beyond range (%ld)",
			  label->sector, LABEL_SCAN_SECTORS);
		return 0;
	}

	memset(buf, 0, LABEL_SIZE);

	strncpy((char *)lh->id, LABEL_ID, sizeof(lh->id));
	lh->sector_xl = xlate64(label->sector);
	lh->offset_xl = xlate32(sizeof(*lh));

	if (!(label->labeller->ops->write)(label, buf))
		return_0;

	lh->crc_xl = xlate32(calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl, LABEL_SIZE -
				      ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)));

	if (!dev_open(dev))
		return_0;

	log_very_verbose("%s: Writing label to sector %" PRIu64 " with stored offset %"
			 PRIu32 ".", dev_name(dev), label->sector,
			 xlate32(lh->offset_xl));
	if (!dev_write(dev, label->sector << SECTOR_SHIFT, LABEL_SIZE, DEV_IO_LABEL, buf)) {
		log_debug_devs("Failed to write label to %s", dev_name(dev));
		r = 0;
	}

	if (!dev_close(dev))
		stack;

	return r;
}

void label_destroy(struct label *label)
{
	label->labeller->ops->destroy_label(label->labeller, label);
	dm_free(label);
}

struct label *label_create(struct labeller *labeller)
{
	struct label *label;

	if (!(label = dm_zalloc(sizeof(*label)))) {
		log_error("label allocaction failed");
		return NULL;
	}

	label->labeller = labeller;

	labeller->ops->initialise_label(labeller, label);

	return label;
}
