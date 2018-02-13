/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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
#include "bcache.h"
#include "toolcontext.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>


/* FIXME Allow for larger labels?  Restricted to single sector currently */

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

/* FIXME Also wipe associated metadata area headers? */
int label_remove(struct device *dev)
{
	char buf[LABEL_SIZE] __attribute__((aligned(8)));
	char readbuf[LABEL_SCAN_SIZE] __attribute__((aligned(8)));
	int r = 1;
	uint64_t sector;
	int wipe;
	struct labeller_i *li;
	struct label_header *lh;
	struct lvmcache_info *info;

	memset(buf, 0, LABEL_SIZE);

	log_very_verbose("Scanning for labels to wipe from %s", dev_name(dev));

	label_scan_invalidate(dev);

	if (!dev_open(dev))
		return_0;

	/*
	 * We flush the device just in case someone is stupid
	 * enough to be trying to import an open pv into lvm.
	 */
	dev_flush(dev);

	if (!dev_read(dev, UINT64_C(0), LABEL_SCAN_SIZE, DEV_IO_LABEL, readbuf)) {
		log_debug_devs("%s: Failed to read label area", dev_name(dev));
		goto out;
	}

	/* Scan first few sectors for anything looking like a label */
	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {
		lh = (struct label_header *) (readbuf +
					      (sector << SECTOR_SHIFT));

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
			if (dev_write(dev, sector << SECTOR_SHIFT, LABEL_SIZE, DEV_IO_LABEL,
				       buf)) {
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

	return r;
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

	label_scan_invalidate(dev);

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


/* global variable for accessing the bcache populated by label scan */
struct bcache *scan_bcache;

#define BCACHE_BLOCK_SIZE_IN_SECTORS 2048 /* 1MB */

static bool _in_bcache(struct device *dev)
{
	if (!dev)
		return NULL;
	return (dev->flags & DEV_IN_BCACHE) ? true : false;
}

static struct labeller *_find_lvm_header(struct device *dev,
				   char *scan_buf,
				   char *label_buf,
				   uint64_t *label_sector,
				   uint64_t scan_sector)
{
	struct labeller_i *li;
	struct labeller *labeller_ret = NULL;
	struct label_header *lh;
	uint64_t sector;
	int found = 0;

	/*
	 * Find which sector in scan_buf starts with a valid label,
	 * and copy it into label_buf.
	 */

	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {
		lh = (struct label_header *) (scan_buf + (sector << SECTOR_SHIFT));

		if (!strncmp((char *)lh->id, LABEL_ID, sizeof(lh->id))) {
			if (found) {
				log_error("Ignoring additional label on %s at sector %llu",
					  dev_name(dev), (unsigned long long)(sector + scan_sector));
			}
			if (xlate64(lh->sector_xl) != sector + scan_sector) {
				log_very_verbose("%s: Label for sector %llu found at sector %llu - ignoring.",
						 dev_name(dev),
						 (unsigned long long)xlate64(lh->sector_xl),
						 (unsigned long long)(sector + scan_sector));
				continue;
			}
			if (calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl, LABEL_SIZE -
				     ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)) !=
			    xlate32(lh->crc_xl)) {
				log_very_verbose("Label checksum incorrect on %s - ignoring", dev_name(dev));
				continue;
			}
			if (found)
				continue;
		}

		dm_list_iterate_items(li, &_labellers) {
			if (li->l->ops->can_handle(li->l, (char *) lh, sector + scan_sector)) {
				log_very_verbose("%s: %s label detected at sector %llu", 
						 dev_name(dev), li->name,
						 (unsigned long long)(sector + scan_sector));
				if (found) {
					log_error("Ignoring additional label on %s at sector %llu",
						  dev_name(dev),
						  (unsigned long long)(sector + scan_sector));
					continue;
				}

				labeller_ret = li->l;
				found = 1;

				memcpy(label_buf, lh, LABEL_SIZE);
				if (label_sector)
					*label_sector = sector + scan_sector;
				break;
			}
		}
	}

	return labeller_ret;
}

/*
 * Process/parse the headers from the data read from a device.
 * Populates lvmcache with device / mda locations / vgname
 * so that vg_read(vgname) will know which devices/locations
 * to read metadata from.
 *
 * If during processing, headers/metadata are found to be needed
 * beyond the range of the scanned block, then additional reads
 * are performed in the processing functions to get that data.
 */
static int _process_block(struct device *dev, struct block *bb, int *is_lvm_device)
{
	char label_buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label *label = NULL;
	struct labeller *labeller;
	struct lvmcache_info *info;
	uint64_t sector;
	int ret = 0;

	/*
	 * Finds the data sector containing the label and copies into label_buf.
	 * label_buf: struct label_header + struct pv_header + struct pv_header_extension
	 *
	 * FIXME: we don't need to copy one sector from bb->data into label_buf,
	 * we can just point label_buf at one sector in ld->buf.
	 */
	if (!(labeller = _find_lvm_header(dev, bb->data, label_buf, &sector, 0))) {

		/*
		 * Non-PVs exit here
		 *
		 * FIXME: check for PVs with errors that also exit here!
		 * i.e. this code cannot distinguish between a non-lvm
		 * device an an lvm device with errors.
		 */

		log_very_verbose("%s: No lvm label detected", dev_name(dev));

		if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
			/* FIXME: if this case is actually happening, fix it. */
			log_warn("Device %s has no label, removing PV info from lvmcache.", dev_name(dev));
			lvmcache_del(info);
		}

		*is_lvm_device = 0;
		goto_out;
	}

	*is_lvm_device = 1;

	/*
	 * This is the point where the scanning code dives into the rest of
	 * lvm.  ops->read() is usually _text_read() which reads the pv_header,
	 * mda locations, mda contents.  As these bits of data are read, they
	 * are saved into lvmcache as info/vginfo structs.
	 */

	if ((ret = (labeller->ops->read)(labeller, dev, label_buf, &label)) && label) {
		label->dev = dev;
		label->sector = sector;
	} else {
		/* FIXME: handle errors */
	}
 out:
	return ret;
}

static int _scan_dev_open(struct device *dev)
{
	const char *name;
	int flags = 0;
	int fd;

	if (!dev)
		return 0;

	if (dev->flags & DEV_IN_BCACHE) {
		log_error("scan_dev_open %s DEV_IN_BCACHE already set", dev_name(dev));
		dev->flags &= ~DEV_IN_BCACHE;
	}

	if (dev->bcache_fd > 0) {
		log_error("scan_dev_open %s already open with fd %d",
			  dev_name(dev), dev->bcache_fd);
		return 0;
	}

	if (!(name = dev_name_confirmed(dev, 1))) {
		log_error("scan_dev_open %s no name", dev_name(dev));
		return 0;
	}

	flags |= O_RDWR;
	flags |= O_DIRECT;
	flags |= O_NOATIME;

	if (dev->flags & DEV_BCACHE_EXCL)
		flags |= O_EXCL;

	fd = open(name, flags, 0777);

	if (fd < 0) {
		if ((errno == EBUSY) && (flags & O_EXCL)) {
			log_error("Can't open %s exclusively.  Mounted filesystem?",
				  dev_name(dev));
		} else {
			log_error("scan_dev_open %s failed errno %d", dev_name(dev), errno);
		}
		return 0;
	}

	dev->flags |= DEV_IN_BCACHE;
	dev->bcache_fd = fd;
	return 1;
}

static int _scan_dev_close(struct device *dev)
{
	if (!(dev->flags & DEV_IN_BCACHE))
		log_error("scan_dev_close %s no DEV_IN_BCACHE set", dev_name(dev));

	dev->flags &= ~DEV_IN_BCACHE;
	dev->flags &= ~DEV_BCACHE_EXCL;

	if (dev->bcache_fd < 0) {
		log_error("scan_dev_close %s already closed", dev_name(dev));
		return 0;
	}

	if (close(dev->bcache_fd))
		log_warn("close %s errno %d", dev_name(dev), errno);
	dev->bcache_fd = -1;
	return 1;
}

/*
 * Read or reread label/metadata from selected devs.
 *
 * Reads and looks at label_header, pv_header, pv_header_extension,
 * mda_header, raw_locns, vg metadata from each device.
 *
 * Effect is populating lvmcache with latest info/vginfo (PV/VG) data
 * from the devs.  If a scanned device does not have a label_header,
 * its info is removed from lvmcache.
 */

static int _scan_list(struct dm_list *devs, int *failed)
{
	struct dm_list wait_devs;
	struct dm_list done_devs;
	struct device_list *devl, *devl2;
	struct block *bb;
	int scan_failed_count = 0;
	int scan_lvm_count = 0;
	int rem_prefetches;
	int scan_failed;
	int is_lvm_device;

	dm_list_init(&wait_devs);
	dm_list_init(&done_devs);

	log_debug_devs("Scanning %d devices.", dm_list_size(devs));

 scan_more:
	rem_prefetches = bcache_max_prefetches(scan_bcache);

	dm_list_iterate_items_safe(devl, devl2, devs) {

		/*
		 * If we prefetch more devs than blocks in the cache, then the
		 * cache will wait for earlier reads to complete, toss the
		 * results, and reuse those blocks before we've had a chance to
		 * use them.  So, prefetch as many as are available, wait for
		 * and process them, then repeat.
		 */
		if (!rem_prefetches)
			break;

		if (!_in_bcache(devl->dev)) {
			if (!_scan_dev_open(devl->dev)) {
				log_debug_devs("%s: Failed to open device.", dev_name(devl->dev));
				dm_list_del(&devl->list);
				scan_failed_count++;
				continue;
			}
		}

		bcache_prefetch(scan_bcache, devl->dev->bcache_fd, 0);

		rem_prefetches--;

		dm_list_del(&devl->list);
		dm_list_add(&wait_devs, &devl->list);
	}

	dm_list_iterate_items_safe(devl, devl2, &wait_devs) {
		bb = NULL;

		if (!bcache_get(scan_bcache, devl->dev->bcache_fd, 0, 0, &bb)) {
			log_debug_devs("%s: Failed to scan device.", dev_name(devl->dev));
			scan_failed_count++;
			scan_failed = 1;
		} else {
			log_debug_devs("Processing data from device %s fd %d block %p", dev_name(devl->dev), devl->dev->bcache_fd, bb);
			_process_block(devl->dev, bb, &is_lvm_device);
			scan_lvm_count++;
			scan_failed = 0;
		}

		if (bb)
			bcache_put(bb);

		/*
		 * Keep the bcache block of lvm devices we have processed so
		 * that the vg_read phase can reuse it.  If bcache failed to
		 * read the block, or the device does not belong to lvm, then
		 * drop it from bcache.
		 */
		if (scan_failed || !is_lvm_device) {
			bcache_invalidate_fd(scan_bcache, devl->dev->bcache_fd);
			_scan_dev_close(devl->dev);
		}

		dm_list_del(&devl->list);
		dm_list_add(&done_devs, &devl->list);
	}

	if (!dm_list_empty(devs))
		goto scan_more;

	log_debug_devs("Scanned %d devices: %d for lvm, %d failed.",
			dm_list_size(&done_devs), scan_lvm_count, scan_failed_count);

	if (failed)
		*failed = scan_failed_count;

	return 1;
}

/*
 * Scan and cache lvm data from all devices on the system.
 * The cache should be empty/reset before calling this.
 */

int label_scan(struct cmd_context *cmd)
{
	struct dm_list all_devs;
	struct dev_iter *iter;
	struct device_list *devl;
	struct device *dev;
	struct io_engine *ioe;
	int cache_blocks;

	log_debug_devs("Finding devices to scan");

	dm_list_init(&all_devs);

	/*
	 * Iterate through all the devices in dev-cache (block devs that appear
	 * under /dev that could possibly hold a PV and are not excluded by
	 * filters).  Read each to see if it's an lvm device, and if so
	 * populate lvmcache with some basic info about the device and the VG
	 * on it.  This info will be used by the vg_read() phase of the
	 * command.
	 */
	dev_cache_full_scan(cmd->full_filter);

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning failed to get devices.");
		return 0;
	}

	while ((dev = dev_iter_get(iter))) {
		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			return 0;
		devl->dev = dev;
		dm_list_add(&all_devs, &devl->list);

		/*
		 * label_scan should not generally be called a second time,
		 * so this will usually not be true.
		 */
		if (_in_bcache(dev)) {
			bcache_invalidate_fd(scan_bcache, dev->bcache_fd);
			_scan_dev_close(dev);
		}
	};
	dev_iter_destroy(iter);

	if (!scan_bcache) {
		/* No devices can happen, just create bcache with any small number. */
		if (!(cache_blocks = dm_list_size(&all_devs)))
			cache_blocks = 8;

		/*
		 * 100 is arbitrary, it's the max number of concurrent aio's
		 * possible, i.e, the number of devices that can be read at
		 * once.  Should this be configurable?
		 */
		if (!(ioe = create_async_io_engine(100)))
			return 0;

		/*
		 * Configure one cache block for each device on the system.
		 * We won't generally need to cache that many because some
		 * of the devs will not be lvm devices, and we don't need
		 * an entry for those.  We might want to change this.
		 */
		if (!(scan_bcache = bcache_create(BCACHE_BLOCK_SIZE_IN_SECTORS, cache_blocks, ioe)))
			return 0;
	}

	_scan_list(&all_devs, NULL);

	return 1;
}

/*
 * Scan and cache lvm data from the listed devices.  If a device is already
 * scanned and cached, this replaces the previously cached lvm data for the
 * device.  This is called when vg_read() wants to guarantee that it is using
 * the latest data from the devices in the VG (since the scan populated bcache
 * without a lock.)
 */

int label_scan_devs(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, devs) {
		if (_in_bcache(devl->dev)) {
			bcache_invalidate_fd(scan_bcache, devl->dev->bcache_fd);
			_scan_dev_close(devl->dev);
		}
	}

	_scan_list(devs, NULL);

	/* FIXME: this function should probably fail if any devs couldn't be scanned */

	return 1;
}

int label_scan_devs_excl(struct dm_list *devs)
{
	struct device_list *devl;
	int failed = 0;

	dm_list_iterate_items(devl, devs) {
		if (_in_bcache(devl->dev)) {
			bcache_invalidate_fd(scan_bcache, devl->dev->bcache_fd);
			_scan_dev_close(devl->dev);
		}
		/*
		 * With this flag set, _scan_dev_open() done by
		 * _scan_list() will do open EXCL
		 */
		devl->dev->flags |= DEV_BCACHE_EXCL;
	}

	_scan_list(devs, &failed);

	if (failed)
		return 0;
	return 1;
}

void label_scan_invalidate(struct device *dev)
{
	if (_in_bcache(dev)) {
		bcache_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);
	}
}

/*
 * Undo label_scan()
 *
 * Close devices that are open because bcache is holding blocks for them.
 * Destroy the bcache.
 */

void label_scan_destroy(struct cmd_context *cmd)
{
	struct dev_iter *iter;
	struct device *dev;

	if (!scan_bcache)
		return;

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		return;
	}

	while ((dev = dev_iter_get(iter)))
		label_scan_invalidate(dev);
	dev_iter_destroy(iter);

	bcache_destroy(scan_bcache);
	scan_bcache = NULL;
}

/*
 * Read (or re-read) and process (or re-process) the data for a device.  This
 * will reset (clear and repopulate) the bcache and lvmcache info for this
 * device.  There are only a couple odd places that want to reread a specific
 * device, this is not a commonly used function.
 */

/* FIXME: remove unused_sector arg */

int label_read(struct device *dev, struct label **labelp, uint64_t unused_sector)
{
	struct dm_list one_dev;
	struct device_list *devl;
	int failed = 0;

	/* scanning is done by list, so make a single item list for this dev */
	if (!(devl = dm_zalloc(sizeof(*devl))))
		return 0;
	devl->dev = dev;
	dm_list_init(&one_dev);
	dm_list_add(&one_dev, &devl->list);

	if (_in_bcache(dev)) {
		bcache_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);
	}

	_scan_list(&one_dev, &failed);

	/*
	 * FIXME: this ugliness of returning a pointer to the label is
	 * temporary until the callers can be updated to not use this.
	 */
	if (labelp) {
		struct lvmcache_info *info;

		info = lvmcache_info_from_pvid(dev->pvid, dev, 1);
		if (info)
			*labelp = lvmcache_get_label(info);
	}

	if (failed)
		return 0;
	return 1;
}

/*
 * Read a label from a specfic, non-zero sector.  This is used in only
 * one place: pvck -> pv_analyze.
 */

int label_read_sector(struct device *dev, struct label **labelp, uint64_t scan_sector)
{
	if (scan_sector) {
		/* TODO: not yet implemented */
		/* When is this done?  When does it make sense?  Is it actually possible? */
		return 0;
	}

	return label_read(dev, labelp, 0);
}

/*
 * FIXME: remove this.  It should not be needed once writes are going through
 * bcache.  As it is now, the write path involves multiple writes to a device,
 * and later writes want to read previous writes from disk.  They do these
 * reads using the standard read paths which require the devs to be in bcache,
 * but the bcache reads do not find the dev because the writes have gone around
 * bcache.  To work around this for now, check if each dev is in bcache before
 * reading it, and if not add it first.
 */

void label_scan_confirm(struct device *dev)
{
	if (!_in_bcache(dev))
		label_read(dev, NULL, 0);
}

