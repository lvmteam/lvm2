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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/label/label.h"
#include "lib/misc/crc.h"
#include "lib/mm/xlate.h"
#include "lib/cache/lvmcache.h"
#include "lib/device/bcache.h"
#include "lib/commands/toolcontext.h"
#include "lib/activate/activate.h"
#include "lib/label/hints.h"
#include "lib/metadata/metadata.h"
#include "lib/format_text/format-text.h"
#include "lib/format_text/layout.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/resource.h>

/* FIXME Allow for larger labels?  Restricted to single sector currently */

static uint64_t _current_bcache_size_bytes;

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

	if (!(li = malloc(len))) {
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
		free(li);
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
	char readbuf[LABEL_SIZE] __attribute__((aligned(8)));
	int r = 1;
	uint64_t sector;
	int wipe;
	struct labeller_i *li;
	struct label_header *lh;
	struct lvmcache_info *info;

	log_very_verbose("Scanning for labels to wipe from %s", dev_name(dev));

	if (!label_scan_open_excl(dev)) {
		log_error("Failed to open device %s", dev_name(dev));
		return 0;
	}

	/* Scan first few sectors for anything looking like a label */
	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {

		memset(readbuf, 0, sizeof(readbuf));

		if (!dev_read_bytes(dev, sector << SECTOR_SHIFT, LABEL_SIZE, readbuf)) {
			log_error("Failed to read label from %s sector %llu",
				  dev_name(dev), (unsigned long long)sector);
			continue;
		}

		lh = (struct label_header *)readbuf;

		wipe = 0;

		if (!memcmp(lh->id, LABEL_ID, sizeof(lh->id))) {
			if (xlate64(lh->sector_xl) == sector)
				wipe = 1;
		} else {
			dm_list_iterate_items(li, &_labellers) {
				if (li->l->ops->can_handle(li->l, (char *)lh, sector)) {
					wipe = 1;
					break;
				}
			}
		}

		if (wipe) {
			log_very_verbose("%s: Wiping label at sector %llu",
					 dev_name(dev), (unsigned long long)sector);

			if (!dev_write_zeros(dev, sector << SECTOR_SHIFT, LABEL_SIZE)) {
				log_error("Failed to remove label from %s at sector %llu",
					  dev_name(dev), (unsigned long long)sector);
				r = 0;
			} else {
				/* Also remove the PV record from cache. */
				info = lvmcache_info_from_pvid(dev->pvid, dev, 0);
				if (info)
					lvmcache_del(info);
			}
		}
	}

	return r;
}

/* Caller may need to use label_get_handler to create label struct! */
int label_write(struct device *dev, struct label *label)
{
	char buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label_header *lh = (struct label_header *) buf;
	uint64_t offset;
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

	memcpy(lh->id, LABEL_ID, sizeof(lh->id));
	lh->sector_xl = xlate64(label->sector);
	lh->offset_xl = xlate32(sizeof(*lh));

	if (!(label->labeller->ops->write)(label, buf))
		return_0;

	lh->crc_xl = xlate32(calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl, LABEL_SIZE -
				      ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)));

	log_very_verbose("%s: Writing label to sector %" PRIu64 " with stored offset %"
			 PRIu32 ".", dev_name(dev), label->sector,
			 xlate32(lh->offset_xl));

	if (!label_scan_open(dev)) {
		log_error("Failed to open device %s", dev_name(dev));
		return 0;
	}

	offset = label->sector << SECTOR_SHIFT;

	dev_set_last_byte(dev, offset + LABEL_SIZE);

	if (!dev_write_bytes(dev, offset, LABEL_SIZE, buf)) {
		log_debug_devs("Failed to write label to %s", dev_name(dev));
		return 0;
	}

	dev_unset_last_byte(dev);

	return r;
}

void label_destroy(struct label *label)
{
	label->labeller->ops->destroy_label(label->labeller, label);
	free(label);
}

struct label *label_create(struct labeller *labeller)
{
	struct label *label;

	if (!(label = zalloc(sizeof(*label)))) {
		log_error("label allocaction failed");
		return NULL;
	}

	label->labeller = labeller;

	labeller->ops->initialise_label(labeller, label);

	return label;
}


/* global variable for accessing the bcache populated by label scan */
struct bcache *scan_bcache;

#define BCACHE_BLOCK_SIZE_IN_SECTORS 256 /* 256*512 = 128K */

static bool _in_bcache(struct device *dev)
{
	if (!dev)
		return NULL;
	return (dev->flags & DEV_IN_BCACHE) ? true : false;
}

static struct labeller *_find_lvm_header(struct device *dev,
				   char *scan_buf,
				   uint32_t scan_buf_sectors,
				   char *label_buf,
				   uint64_t *label_sector,
				   uint64_t block_sector,
				   uint64_t start_sector)
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

	for (sector = start_sector; sector < start_sector + LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {

		/*
		 * The scan_buf passed in is a bcache block, which is
		 * BCACHE_BLOCK_SIZE_IN_SECTORS large.  So if start_sector is
		 * one of the last couple sectors in that buffer, we need to
		 * break early.
		 */
		if (sector >= scan_buf_sectors)
			break;

		lh = (struct label_header *) (scan_buf + (sector << SECTOR_SHIFT));

		if (!memcmp(lh->id, LABEL_ID, sizeof(lh->id))) {
			if (found) {
				log_error("Ignoring additional label on %s at sector %llu",
					  dev_name(dev), (unsigned long long)(block_sector + sector));
			}
			if (xlate64(lh->sector_xl) != sector) {
				log_warn("%s: Label for sector %llu found at sector %llu - ignoring.",
					 dev_name(dev),
					 (unsigned long long)xlate64(lh->sector_xl),
					 (unsigned long long)(block_sector + sector));
				continue;
			}
			if (calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl,
				     LABEL_SIZE - ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)) != xlate32(lh->crc_xl)) {
				log_very_verbose("Label checksum incorrect on %s - ignoring", dev_name(dev));
				continue;
			}
			if (found)
				continue;
		}

		dm_list_iterate_items(li, &_labellers) {
			if (li->l->ops->can_handle(li->l, (char *) lh, block_sector + sector)) {
				log_very_verbose("%s: %s label detected at sector %llu", 
						 dev_name(dev), li->name,
						 (unsigned long long)(block_sector + sector));
				if (found) {
					log_error("Ignoring additional label on %s at sector %llu",
						  dev_name(dev),
						  (unsigned long long)(block_sector + sector));
					continue;
				}

				labeller_ret = li->l;
				found = 1;

				memcpy(label_buf, lh, LABEL_SIZE);
				if (label_sector)
					*label_sector = block_sector + sector;
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
static int _process_block(struct cmd_context *cmd, struct dev_filter *f,
			  struct device *dev, struct block *bb,
			  uint64_t block_sector, uint64_t start_sector,
			  int *is_lvm_device)
{
	char label_buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct labeller *labeller;
	uint64_t sector = 0;
	int is_duplicate = 0;
	int ret = 0;
	int pass;

	dev->flags &= ~DEV_SCAN_FOUND_LABEL;

	/*
	 * The device may have signatures that exclude it from being processed.
	 * If filters were applied before bcache data was available, some
	 * filters may have deferred their check until the point where bcache
	 * data had been read (here).  They set this flag to indicate that the
	 * filters should be retested now that data from the device is ready.
	 */
	if (f && (dev->flags & DEV_FILTER_AFTER_SCAN)) {
		dev->flags &= ~DEV_FILTER_AFTER_SCAN;

		log_debug_devs("Scan filtering %s", dev_name(dev));
		
		pass = f->passes_filter(cmd, f, dev, NULL);

		if ((pass == -EAGAIN) || (dev->flags & DEV_FILTER_AFTER_SCAN)) {
			/* Shouldn't happen */
			dev->flags &= ~DEV_FILTER_OUT_SCAN;
			log_debug_devs("Scan filter should not be deferred %s", dev_name(dev));
			pass = 1;
		}

		if (!pass) {
			log_very_verbose("%s: Not processing filtered", dev_name(dev));
			dev->flags |= DEV_FILTER_OUT_SCAN;
			*is_lvm_device = 0;
			goto_out;
		}
	}

	/*
	 * Finds the data sector containing the label and copies into label_buf.
	 * label_buf: struct label_header + struct pv_header + struct pv_header_extension
	 *
	 * FIXME: we don't need to copy one sector from bb->data into label_buf,
	 * we can just point label_buf at one sector in ld->buf.
	 */
	if (!(labeller = _find_lvm_header(dev, bb->data, BCACHE_BLOCK_SIZE_IN_SECTORS, label_buf, &sector, block_sector, start_sector))) {

		/*
		 * Non-PVs exit here
		 *
		 * FIXME: check for PVs with errors that also exit here!
		 * i.e. this code cannot distinguish between a non-lvm
		 * device an an lvm device with errors.
		 */

		log_very_verbose("%s: No lvm label detected", dev_name(dev));

		lvmcache_del_dev(dev); /* FIXME: if this is needed, fix it. */

		*is_lvm_device = 0;
		goto_out;
	}

	dev->flags |= DEV_SCAN_FOUND_LABEL;
	*is_lvm_device = 1;

	/*
	 * This is the point where the scanning code dives into the rest of
	 * lvm.  ops->read() is _text_read() which reads the pv_header, mda
	 * locations, and metadata text.  All of the info it finds about the PV
	 * and VG is stashed in lvmcache which saves it in the form of
	 * info/vginfo structs.  That lvmcache info is used later when the
	 * command wants to read the VG to do something to it.
	 */
	ret = labeller->ops->read(labeller, dev, label_buf, sector, &is_duplicate);

	if (!ret) {
		if (is_duplicate) {
			/*
			 * _text_read() called lvmcache_add() which found an
			 * existing info struct for this PVID but for a
			 * different dev.  lvmcache_add() did not add an info
			 * struct for this dev, but added this dev to the list
			 * of duplicate devs.
			 */
			log_debug("label scan found duplicate PVID %s on %s", dev->pvid, dev_name(dev));
		} else {
			/*
			 * Leave the info in lvmcache because the device is
			 * present and can still be used even if it has
			 * metadata that we can't process (we can get metadata
			 * from another PV/mda.) _text_read only saves mdas
			 * with good metadata in lvmcache (this includes old
			 * metadata), and if a PV has no mdas with good
			 * metadata, then the info for the PV will be in
			 * lvmcache with empty info->mdas, and it will behave
			 * like a PV with no mdas (a common configuration.)
			 */
			log_warn("WARNING: scan failed to get metadata summary from %s PVID %s", dev_name(dev), dev->pvid);
		}
	}
 out:
	return ret;
}

static int _scan_dev_open(struct device *dev)
{
	struct dm_list *name_list;
	struct dm_str_list *name_sl;
	const char *name;
	struct stat sbuf;
	int retried = 0;
	int flags = 0;
	int fd;

	if (!dev)
		return 0;

	if (dev->flags & DEV_IN_BCACHE) {
		/* Shouldn't happen */
		log_error("Device open %s has DEV_IN_BCACHE already set", dev_name(dev));
		dev->flags &= ~DEV_IN_BCACHE;
	}

	if (dev->bcache_fd > 0) {
		/* Shouldn't happen */
		log_error("Device open %s already open with fd %d",
			  dev_name(dev), dev->bcache_fd);
		return 0;
	}

	/*
	 * All the names for this device (major:minor) are kept on
	 * dev->aliases, the first one is the primary/preferred name.
	 */
	if (!(name_list = dm_list_first(&dev->aliases))) {
		/* Shouldn't happen */
		log_error("Device open %s %d:%d has no path names.",
			  dev_name(dev), (int)MAJOR(dev->dev), (int)MINOR(dev->dev));
		return 0;
	}
	name_sl = dm_list_item(name_list, struct dm_str_list);
	name = name_sl->str;

	flags |= O_DIRECT;
	flags |= O_NOATIME;

	/*
	 * FIXME: udev is a train wreck when we open RDWR and close, so we
	 * need to only use RDWR when we actually need to write, and use
	 * RDONLY otherwise.  Fix, disable or scrap udev nonsense so we can
	 * just open with RDWR by default.
	 */

	if (dev->flags & DEV_BCACHE_EXCL) {
		flags |= O_EXCL;
		flags |= O_RDWR;
	} else if (dev->flags & DEV_BCACHE_WRITE) {
		flags |= O_RDWR;
	} else {
		flags |= O_RDONLY;
	}

retry_open:

	fd = open(name, flags, 0777);

	if (fd < 0) {
		if ((errno == EBUSY) && (flags & O_EXCL)) {
			log_error("Can't open %s exclusively.  Mounted filesystem?",
				  dev_name(dev));
		} else {
			int major, minor;

			/*
			 * Shouldn't happen, if it does, print stat info to help figure
			 * out what's wrong.
			 */

			major = (int)MAJOR(dev->dev);
			minor = (int)MINOR(dev->dev);

			log_error("Device open %s %d:%d failed errno %d", name, major, minor, errno);

			if (stat(name, &sbuf)) {
				log_debug_devs("Device open %s %d:%d stat failed errno %d",
					       name, major, minor, errno);
			} else if (sbuf.st_rdev != dev->dev) {
				log_debug_devs("Device open %s %d:%d stat %d:%d does not match.",
					       name, major, minor,
					       (int)MAJOR(sbuf.st_rdev), (int)MINOR(sbuf.st_rdev));
			}

			if (!retried) {
				/*
				 * FIXME: remove this, the theory for this retry is that
				 * there may be a udev race that we can sometimes mask by
				 * retrying.  This is here until we can figure out if it's
				 * needed and if so fix the real problem.
				 */
				usleep(5000);
				log_debug_devs("Device open %s retry", dev_name(dev));
				retried = 1;
				goto retry_open;
			}
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

static void _drop_bad_aliases(struct device *dev)
{
	struct dm_str_list *strl, *strl2;
	const char *name;
	struct stat sbuf;
	int major = (int)MAJOR(dev->dev);
	int minor = (int)MINOR(dev->dev);
	int bad;

	dm_list_iterate_items_safe(strl, strl2, &dev->aliases) {
		name = strl->str;
		bad = 0;

		if (stat(name, &sbuf)) {
			bad = 1;
			log_debug_devs("Device path check %d:%d %s stat failed errno %d",
					major, minor, name, errno);
		} else if (sbuf.st_rdev != dev->dev) {
			bad = 1;
			log_debug_devs("Device path check %d:%d %s stat %d:%d does not match.",
				       major, minor, name,
				       (int)MAJOR(sbuf.st_rdev), (int)MINOR(sbuf.st_rdev));
		}

		if (bad) {
			log_debug_devs("Device path check %d:%d dropping path %s.", major, minor, name);
			dev_cache_failed_path(dev, name);
		}
	}
}

// Like bcache_invalidate, only it throws any dirty data away if the
// write fails.
static void _invalidate_fd(struct bcache *cache, int fd)
{
	if (!bcache_invalidate_fd(cache, fd))
		bcache_abort_fd(cache, fd);
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

static int _scan_list(struct cmd_context *cmd, struct dev_filter *f,
		      struct dm_list *devs, int *failed)
{
	struct dm_list wait_devs;
	struct dm_list done_devs;
	struct dm_list reopen_devs;
	struct device_list *devl, *devl2;
	struct block *bb;
	int retried_open = 0;
	int scan_read_errors = 0;
	int scan_process_errors = 0;
	int scan_failed_count = 0;
	int rem_prefetches;
	int submit_count;
	int scan_failed;
	int is_lvm_device;
	int ret;

	dm_list_init(&wait_devs);
	dm_list_init(&done_devs);
	dm_list_init(&reopen_devs);

	log_debug_devs("Scanning %d devices for VG info", dm_list_size(devs));

 scan_more:
	rem_prefetches = bcache_max_prefetches(scan_bcache);
	submit_count = 0;

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
				log_debug_devs("Scan failed to open %s.", dev_name(devl->dev));
				dm_list_del(&devl->list);
				dm_list_add(&reopen_devs, &devl->list);
				continue;
			}
		}

		bcache_prefetch(scan_bcache, devl->dev->bcache_fd, 0);

		rem_prefetches--;
		submit_count++;

		dm_list_del(&devl->list);
		dm_list_add(&wait_devs, &devl->list);
	}

	log_debug_devs("Scanning submitted %d reads", submit_count);

	dm_list_iterate_items_safe(devl, devl2, &wait_devs) {
		bb = NULL;
		scan_failed = 0;
		is_lvm_device = 0;

		if (!bcache_get(scan_bcache, devl->dev->bcache_fd, 0, 0, &bb)) {
			log_debug_devs("Scan failed to read %s.", dev_name(devl->dev));
			scan_failed = 1;
			scan_read_errors++;
			scan_failed_count++;
			lvmcache_del_dev(devl->dev);
		} else {
			log_debug_devs("Processing data from device %s %d:%d fd %d block %p",
				       dev_name(devl->dev),
				       (int)MAJOR(devl->dev->dev),
				       (int)MINOR(devl->dev->dev),
				       devl->dev->bcache_fd, bb);

			ret = _process_block(cmd, f, devl->dev, bb, 0, 0, &is_lvm_device);

			if (!ret && is_lvm_device) {
				log_debug_devs("Scan failed to process %s", dev_name(devl->dev));
				scan_failed = 1;
				scan_process_errors++;
				scan_failed_count++;
			}
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
			_invalidate_fd(scan_bcache, devl->dev->bcache_fd);
			_scan_dev_close(devl->dev);
		}

		dm_list_del(&devl->list);
		dm_list_add(&done_devs, &devl->list);
	}

	if (!dm_list_empty(devs))
		goto scan_more;

	/*
	 * We're done scanning all the devs.  If we failed to open any of them
	 * the first time through, refresh device paths and retry.  We failed
	 * to open the devs on the reopen_devs list.
	 *
	 * FIXME: it's not clear if or why this helps.
	 */
	if (!dm_list_empty(&reopen_devs)) {
		if (retried_open) {
			/* Don't try again. */
			scan_failed_count += dm_list_size(&reopen_devs);
			dm_list_splice(&done_devs, &reopen_devs);
			goto out;
		}
		retried_open = 1;

		dm_list_iterate_items_safe(devl, devl2, &reopen_devs) {
			_drop_bad_aliases(devl->dev);

			if (dm_list_empty(&devl->dev->aliases)) {
				log_warn("WARNING: Scan ignoring device %d:%d with no paths.",
					 (int)MAJOR(devl->dev->dev),
					 (int)MINOR(devl->dev->dev));
					 
				dm_list_del(&devl->list);
				lvmcache_del_dev(devl->dev);
				scan_failed_count++;
			}
		}

		/*
		 * This will search the system's /dev for new path names and
		 * could help us reopen the device if it finds a new preferred
		 * path name for this dev's major:minor.  It does that by
		 * inserting a new preferred path name on dev->aliases.  open
		 * uses the first name from that list.
		 */
		log_debug_devs("Scanning refreshing device paths.");
		dev_cache_scan();

		/* Put devs that failed to open back on the original list to retry. */
		dm_list_splice(devs, &reopen_devs);
		goto scan_more;
	}
out:
	log_debug_devs("Scanned devices: read errors %d process errors %d failed %d",
			scan_read_errors, scan_process_errors, scan_failed_count);

	if (failed)
		*failed = scan_failed_count;

	dm_list_splice(devs, &done_devs);

	return 1;
}

/*
 * We don't know ahead of time if we will find some VG metadata 
 * that is larger than the total size of the bcache, which would
 * prevent us from reading/writing the VG since we do not dynamically
 * increase the bcache size when we find it's too small.  In these
 * cases the user would need to set io_memory_size to be larger
 * than the max VG metadata size (lvm does not impose any limit on
 * the metadata size.)
 */

#define MIN_BCACHE_BLOCKS 32    /* 4MB (32 * 128KB) */
#define MAX_BCACHE_BLOCKS 4096  /* 512MB (4096 * 128KB) */

static int _setup_bcache(void)
{
	struct io_engine *ioe = NULL;
	int iomem_kb = io_memory_size();
	int block_size_kb = (BCACHE_BLOCK_SIZE_IN_SECTORS * 512) / 1024;
	int cache_blocks;

	cache_blocks = iomem_kb / block_size_kb;

	if (cache_blocks < MIN_BCACHE_BLOCKS)
		cache_blocks = MIN_BCACHE_BLOCKS;

	if (cache_blocks > MAX_BCACHE_BLOCKS)
		cache_blocks = MAX_BCACHE_BLOCKS;

	_current_bcache_size_bytes = cache_blocks * BCACHE_BLOCK_SIZE_IN_SECTORS * 512;

	if (use_aio()) {
		if (!(ioe = create_async_io_engine())) {
			log_warn("Failed to set up async io, using sync io.");
			init_use_aio(0);
		}
	}

	if (!ioe) {
		if (!(ioe = create_sync_io_engine())) {
			log_error("Failed to set up sync io.");
			return 0;
		}
	}

	if (!(scan_bcache = bcache_create(BCACHE_BLOCK_SIZE_IN_SECTORS, cache_blocks, ioe))) {
		log_error("Failed to create bcache with %d cache blocks.", cache_blocks);
		return 0;
	}

	return 1;
}

static void _free_hints(struct dm_list *hints)
{
	struct hint *hint, *hint2;

	dm_list_iterate_items_safe(hint, hint2, hints) {
		dm_list_del(&hint->list);
		free(hint);
	}
}

/*
 * We don't know how many of num_devs will be PVs that we need to
 * keep open, but if it's greater than the soft limit, then we'll
 * need the soft limit raised, so do that before starting.
 *
 * If opens approach the raised soft/hard limit while scanning, then
 * we could also attempt to raise the soft/hard limits during the scan.
 */

#define BASE_FD_COUNT 32 /* Number of open files we want apart from devs */

static void _prepare_open_file_limit(struct cmd_context *cmd, unsigned int num_devs)
{
#ifdef HAVE_PRLIMIT
	struct rlimit old, new;
	unsigned int want = num_devs + BASE_FD_COUNT;
	int rv;

	rv = prlimit(0, RLIMIT_NOFILE, NULL, &old);
	if (rv < 0) {
		log_debug("Checking fd limit for num_devs %u failed %d", num_devs, errno);
		return;
	}

	log_debug("Checking fd limit for num_devs %u want %u soft %lld hard %lld",
		  num_devs, want, (long long)old.rlim_cur, (long long)old.rlim_max);

	/* Current soft limit is enough */
	if (old.rlim_cur > want)
		return;

	/* Soft limit already raised to max */
	if (old.rlim_cur == old.rlim_max)
		return;

	/* Raise soft limit up to hard/max limit */
	new.rlim_cur = old.rlim_max;
	new.rlim_max = old.rlim_max;

	log_debug("Setting fd limit for num_devs %u soft %lld hard %lld",
		  num_devs, (long long)new.rlim_cur, (long long)new.rlim_max);

	rv = prlimit(0, RLIMIT_NOFILE, &new, &old);
	if (rv < 0) {
		if (errno == EPERM)
			log_warn("WARNING: permission error setting open file limit for scanning %u devices.", num_devs);
		else
			log_warn("WARNING: cannot set open file limit for scanning %u devices.", num_devs);
		return;
	}
#endif
}

int label_scan_for_pvid(struct cmd_context *cmd, char *pvid, struct device **dev_out)
{
	char buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct dm_list devs;
	struct dev_iter *iter;
	struct device_list *devl, *devl2;
	struct device *dev;
	struct pv_header *pvh;
	int ret = 0;

	dm_list_init(&devs);

	dev_cache_scan();

	if (!(iter = dev_iter_create(cmd->filter, 0))) {
		log_error("Scanning failed to get devices.");
		return 0;
	}

	log_debug_devs("Filtering devices to scan");

	while ((dev = dev_iter_get(cmd, iter))) {
		if (!(devl = zalloc(sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(&devs, &devl->list);
	};
	dev_iter_destroy(iter);

	if (!scan_bcache) {
		if (!_setup_bcache())
			goto_out;
	}

	log_debug_devs("Reading labels for pvid");

	dm_list_iterate_items(devl, &devs) {
		dev = devl->dev;

		memset(buf, 0, sizeof(buf));

		if (!label_scan_open(dev))
			continue;

		if (!dev_read_bytes(dev, 512, LABEL_SIZE, buf)) {
			_scan_dev_close(dev);
			goto out;
		}

		pvh = (struct pv_header *)(buf + 32);

		if (!memcmp(pvh->pv_uuid, pvid, ID_LEN)) {
			*dev_out = devl->dev;
			_scan_dev_close(dev);
			break;
		}

		_scan_dev_close(dev);
	}
	ret = 1;
 out:
	dm_list_iterate_items_safe(devl, devl2, &devs) {
		dm_list_del(&devl->list);
		free(devl);
	}
	return ret;
}

/*
 * Scan devices on the system to discover which are LVM devices.
 * Info about the LVM devices (PVs) is saved in lvmcache in a
 * basic/summary form (info/vginfo structs).  The vg_read phase
 * uses this summary info to know which PVs to look at for
 * processing a given VG.
 */

int label_scan(struct cmd_context *cmd)
{
	struct dm_list all_devs;
	struct dm_list scan_devs;
	struct dm_list hints_list;
	struct dev_iter *iter;
	struct device_list *devl, *devl2;
	struct device *dev;
	uint64_t max_metadata_size_bytes;
	int using_hints;
	int create_hints = 0; /* NEWHINTS_NONE */

	log_debug_devs("Finding devices to scan");

	dm_list_init(&all_devs);
	dm_list_init(&scan_devs);
	dm_list_init(&hints_list);

	/*
	 * dev_cache_scan() creates a list of devices on the system
	 * (saved in in dev-cache) which we can iterate through to
	 * search for LVM devs.  The dev cache list either comes from
	 * looking at dev nodes under /dev, or from udev.
	 */
	dev_cache_scan();

	/*
	 * If we know that there will be md components with an end
	 * superblock, then enable the full md filter before label
	 * scan begins.  FIXME: we could skip the full md check on
	 * devs that are not identified as PVs, but then we'd need
	 * to do something other than using the standard md filter.
	 */
	if (cmd->md_component_detection && !cmd->use_full_md_check &&
	    !strcmp(cmd->md_component_checks, "auto") &&
	    dev_cache_has_md_with_end_superblock(cmd->dev_types)) {
		log_debug("Enable full md component check.");
		cmd->use_full_md_check = 1;
	}

	/*
	 * Set up the iterator that is needed to step through each device in
	 * dev cache.
	 */
	if (!(iter = dev_iter_create(cmd->filter, 0))) {
		log_error("Scanning failed to get devices.");
		return 0;
	}

	log_debug_devs("Filtering devices to scan");

	/*
	 * Iterate through all devices in dev cache and apply filters
	 * to exclude devs that we do not need to scan.  Those devs
	 * that pass the filters are returned by the iterator and
	 * saved in a list of devs that we will proceed to scan to
	 * check if they are LVM devs.  IOW this loop is the
	 * application of filters (those that do not require reading
	 * the devs) to the list of all devices.  It does that because
	 * the 'cmd->filter' is used above when setting up the iterator.
	 * Unfortunately, it's not obvious that this is what's happening
	 * here.  filters that require reading the device are not applied
	 * here, but in process_block(), see DEV_FILTER_AFTER_SCAN.
	 */
	while ((dev = dev_iter_get(cmd, iter))) {
		if (!(devl = zalloc(sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(&all_devs, &devl->list);

		/*
		 * label_scan should not generally be called a second time,
		 * so this will usually not be true.
		 */
		if (_in_bcache(dev)) {
			_invalidate_fd(scan_bcache, dev->bcache_fd);
			_scan_dev_close(dev);
		}
	};
	dev_iter_destroy(iter);

	if (!scan_bcache) {
		if (!_setup_bcache())
			return 0;
	}

	/*
	 * In some common cases we can avoid scanning all devices
	 * by using hints which tell us which devices are PVs, which
	 * are the only devices we actually need to scan.  Without
	 * hints we need to scan all devs to find which are PVs.
	 *
	 * TODO: if the command is using hints and a single vgname
	 * arg, we can also take the vg lock here, prior to scanning.
	 * This means we would not need to rescan the PVs in the VG
	 * in vg_read (skip lvmcache_label_rescan_vg) after the
	 * vg lock is usually taken.  (Some commands are already
	 * able to avoid rescan in vg_read, but locking early would
	 * apply to more cases.)
	 */
	if (!get_hints(cmd, &hints_list, &create_hints, &all_devs, &scan_devs)) {
		dm_list_splice(&scan_devs, &all_devs);
		dm_list_init(&hints_list);
		using_hints = 0;
	} else
		using_hints = 1;

	log_debug("Will scan %d devices skip %d", dm_list_size(&scan_devs), dm_list_size(&all_devs));

	/*
	 * If the total number of devices exceeds the soft open file
	 * limit, then increase the soft limit to the hard/max limit
	 * in case the number of PVs in scan_devs (it's only the PVs
	 * which we want to keep open) is higher than the current
	 * soft limit.
	 */
	_prepare_open_file_limit(cmd, dm_list_size(&scan_devs));

	/*
	 * Do the main scan.
	 */
	_scan_list(cmd, cmd->filter, &scan_devs, NULL);

	/*
	 * Metadata could be larger than total size of bcache, and bcache
	 * cannot currently be resized during the command.  If this is the
	 * case (or within reach), warn that io_memory_size needs to be
	 * set larger.
	 *
	 * Even if bcache out of space did not cause a failure during scan, it
	 * may cause a failure during the next vg_read phase or during vg_write.
	 *
	 * If there was an error during scan, we could recreate bcache here
	 * with a larger size and then restart label_scan.  But, this does not
	 * address the problem of writing new metadata that excedes the bcache
	 * size and failing, which would often be hit first, i.e. we'll fail
	 * to write new metadata exceding the max size before we have a chance
	 * to read any metadata with that size, unless we find an existing vg
	 * that has been previously created with the larger size.
	 *
	 * If the largest metadata is within 1MB of the bcache size, then start
	 * warning.
	 */
	max_metadata_size_bytes = lvmcache_max_metadata_size();

	if (max_metadata_size_bytes + (1024 * 1024) > _current_bcache_size_bytes) {
		/* we want bcache to be 1MB larger than the max metadata seen */
		uint64_t want_size_kb = (max_metadata_size_bytes / 1024) + 1024;
		uint64_t remainder;
		if ((remainder = (want_size_kb % 1024)))
			want_size_kb = want_size_kb + 1024 - remainder;

		log_warn("WARNING: metadata may not be usable with current io_memory_size %d KiB",
			 io_memory_size());
		log_warn("WARNING: increase lvm.conf io_memory_size to at least %llu KiB",
			 (unsigned long long)want_size_kb);
	}

	dm_list_init(&cmd->hints);

	/*
	 * If we're using hints to limit which devs we scanned, verify
	 * that those hints were valid, and if not we need to scan the
	 * rest of the devs.
	 */
	if (using_hints) {
		if (!validate_hints(cmd, &hints_list)) {
			log_debug("Will scan %d remaining devices", dm_list_size(&all_devs));
			_scan_list(cmd, cmd->filter, &all_devs, NULL);
			_free_hints(&hints_list);
			using_hints = 0;
			create_hints = 0;
		} else {
			/* The hints may be used by another device iteration. */
			dm_list_splice(&cmd->hints, &hints_list);
		}
	}

	/*
	 * Stronger exclusion of md components that might have been
	 * misidentified as PVs due to having an end-of-device md superblock.
	 * If we're not using hints, and are not already doing a full md check
	 * on devs being scanned, then if udev info is missing for a PV, scan
	 * the end of the PV to verify it's not an md component.  The full
	 * dev_is_md_component call will do new reads at the end of the dev.
	 */
	if (cmd->md_component_detection && !cmd->use_full_md_check && !using_hints &&
	    !strcmp(cmd->md_component_checks, "auto")) {
		int once = 0;
		dm_list_iterate_items(devl, &scan_devs) {
			if (!(devl->dev->flags & DEV_SCAN_FOUND_LABEL))
				continue;
			if (!(devl->dev->flags & DEV_UDEV_INFO_MISSING))
				continue;
			if (!once++)
				log_debug_devs("Scanning end of PVs with no udev info for MD components");

			if (dev_is_md_component(devl->dev, NULL, 1)) {
				log_debug_devs("Scan dropping PV from MD component %s", dev_name(devl->dev));
				devl->dev->flags &= ~DEV_SCAN_FOUND_LABEL;
				lvmcache_del_dev(devl->dev);
				lvmcache_del_dev_from_duplicates(devl->dev);
			}
		}
	}

	dm_list_iterate_items_safe(devl, devl2, &all_devs) {
		dm_list_del(&devl->list);
		free(devl);
	}

	dm_list_iterate_items_safe(devl, devl2, &scan_devs) {
		dm_list_del(&devl->list);
		free(devl);
	}

	/*
	 * If hints were not available/usable, then we scanned all devs,
	 * and we now know which are PVs.  Save this list of PVs we've
	 * identified as hints for the next command to use.
	 * (create_hints variable has NEWHINTS_X value which indicates
	 * the reason for creating the new hints.)
	 */
	if (create_hints)
		write_hint_file(cmd, create_hints);

	return 1;
}

/*
 * Scan and cache lvm data from the listed devices.  If a device is already
 * scanned and cached, this replaces the previously cached lvm data for the
 * device.  This is called when vg_read() wants to guarantee that it is using
 * the latest data from the devices in the VG (since the scan populated bcache
 * without a lock.)
 */

int label_scan_devs(struct cmd_context *cmd, struct dev_filter *f, struct dm_list *devs)
{
	struct device_list *devl;

	if (!scan_bcache) {
		if (!_setup_bcache())
			return 0;
	}

	dm_list_iterate_items(devl, devs) {
		if (_in_bcache(devl->dev)) {
			_invalidate_fd(scan_bcache, devl->dev->bcache_fd);
			_scan_dev_close(devl->dev);
		}
	}

	_scan_list(cmd, f, devs, NULL);

	return 1;
}

/*
 * This function is used when the caller plans to write to the devs, so opening
 * them RW during rescan avoids needing to close and reopen with WRITE in
 * dev_write_bytes.
 */

int label_scan_devs_rw(struct cmd_context *cmd, struct dev_filter *f, struct dm_list *devs)
{
	struct device_list *devl;

	if (!scan_bcache) {
		if (!_setup_bcache())
			return 0;
	}

	dm_list_iterate_items(devl, devs) {
		if (_in_bcache(devl->dev)) {
			_invalidate_fd(scan_bcache, devl->dev->bcache_fd);
			_scan_dev_close(devl->dev);
		}
		/*
		 * With this flag set, _scan_dev_open() done by
		 * _scan_list() will do open RW
		 */
		devl->dev->flags |= DEV_BCACHE_WRITE;
	}

	_scan_list(cmd, f, devs, NULL);

	return 1;
}

int label_scan_devs_excl(struct dm_list *devs)
{
	struct device_list *devl;
	int failed = 0;

	dm_list_iterate_items(devl, devs) {
		if (_in_bcache(devl->dev)) {
			_invalidate_fd(scan_bcache, devl->dev->bcache_fd);
			_scan_dev_close(devl->dev);
		}
		/*
		 * With this flag set, _scan_dev_open() done by
		 * _scan_list() will do open EXCL
		 */
		devl->dev->flags |= DEV_BCACHE_EXCL;
	}

	_scan_list(NULL, NULL, devs, &failed);

	if (failed)
		return 0;
	return 1;
}

void label_scan_invalidate(struct device *dev)
{
	if (_in_bcache(dev)) {
		_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);
	}
}

/*
 * If a PV is stacked on an LV, then the LV is kept open
 * in bcache, and needs to be closed so the open fd doesn't
 * interfere with processing the LV.
 */

void label_scan_invalidate_lv(struct cmd_context *cmd, struct logical_volume *lv)
{
	struct lvinfo lvinfo;
	struct device *dev;
	dev_t devt;

	if (!lv_info(cmd, lv, 0, &lvinfo, 0, 0))
		return;

	devt = MKDEV(lvinfo.major, lvinfo.minor);
	if ((dev = dev_cache_get_by_devt(cmd, devt, NULL, NULL)))
		label_scan_invalidate(dev);
}

/*
 * Empty the bcache of all blocks and close all open fds,
 * but keep the bcache set up.
 */

void label_scan_drop(struct cmd_context *cmd)
{
	struct dev_iter *iter;
	struct device *dev;

	if (!(iter = dev_iter_create(NULL, 0)))
		return;

	while ((dev = dev_iter_get(cmd, iter))) {
		if (_in_bcache(dev))
			_scan_dev_close(dev);
	}
	dev_iter_destroy(iter);
}

/*
 * Close devices that are open because bcache is holding blocks for them.
 * Destroy the bcache.
 */

void label_scan_destroy(struct cmd_context *cmd)
{
	if (!scan_bcache)
		return;

	label_scan_drop(cmd);

	bcache_destroy(scan_bcache);
	scan_bcache = NULL;
}

/*
 * Read (or re-read) and process (or re-process) the data for a device.  This
 * will reset (clear and repopulate) the bcache and lvmcache info for this
 * device.  There are only a couple odd places that want to reread a specific
 * device, this is not a commonly used function.
 */

int label_read(struct device *dev)
{
	struct dm_list one_dev;
	struct device_list *devl;
	int failed = 0;

	/* scanning is done by list, so make a single item list for this dev */
	if (!(devl = zalloc(sizeof(*devl))))
		return 0;
	devl->dev = dev;
	dm_list_init(&one_dev);
	dm_list_add(&one_dev, &devl->list);

	if (_in_bcache(dev)) {
		_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);
	}

	_scan_list(NULL, NULL, &one_dev, &failed);

	free(devl);

	if (failed)
		return 0;
	return 1;
}

int label_scan_setup_bcache(void)
{
	if (!scan_bcache) {
		if (!_setup_bcache())
			return 0;
	}

	return 1;
}

/*
 * This is needed to write to a new non-lvm device.
 * Scanning that dev would not keep it open or in
 * bcache, but to use bcache_write we need the dev
 * to be open so we can use dev->bcache_fd to write.
 */

int label_scan_open(struct device *dev)
{
	if (!_in_bcache(dev))
		return _scan_dev_open(dev);
	return 1;
}

int label_scan_open_excl(struct device *dev)
{
	if (_in_bcache(dev) && !(dev->flags & DEV_BCACHE_EXCL)) {
		/* FIXME: avoid tossing out bcache blocks just to replace fd. */
		log_debug("Close and reopen excl %s", dev_name(dev));
		_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);
	}
	dev->flags |= DEV_BCACHE_EXCL;
	dev->flags |= DEV_BCACHE_WRITE;
	return label_scan_open(dev);
}

int label_scan_open_rw(struct device *dev)
{
	if (_in_bcache(dev) && !(dev->flags & DEV_BCACHE_WRITE)) {
		/* FIXME: avoid tossing out bcache blocks just to replace fd. */
		log_debug("Close and reopen rw %s", dev_name(dev));
		_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);
	}
	dev->flags |= DEV_BCACHE_WRITE;
	return label_scan_open(dev);
}

bool dev_read_bytes(struct device *dev, uint64_t start, size_t len, void *data)
{
	if (!scan_bcache) {
		/* Should not happen */
		log_error("dev_read bcache not set up %s", dev_name(dev));
		return false;
	}

	if (dev->bcache_fd <= 0) {
		/* This is not often needed. */
		if (!label_scan_open(dev)) {
			log_error("Error opening device %s for reading at %llu length %u.",
				  dev_name(dev), (unsigned long long)start, (uint32_t)len);
			return false;
		}
	}

	if (!bcache_read_bytes(scan_bcache, dev->bcache_fd, start, len, data)) {
		log_error("Error reading device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		label_scan_invalidate(dev);
		return false;
	}
	return true;

}

bool dev_write_bytes(struct device *dev, uint64_t start, size_t len, void *data)
{
	if (test_mode())
		return true;

	if (!scan_bcache) {
		/* Should not happen */
		log_error("dev_write bcache not set up %s", dev_name(dev));
		return false;
	}

	if (_in_bcache(dev) && !(dev->flags & DEV_BCACHE_WRITE)) {
		/* FIXME: avoid tossing out bcache blocks just to replace fd. */
		log_debug("Close and reopen to write %s", dev_name(dev));
		_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);

		dev->flags |= DEV_BCACHE_WRITE;
		label_scan_open(dev);
	}

	if (dev->bcache_fd <= 0) {
		/* This is not often needed. */
		dev->flags |= DEV_BCACHE_WRITE;
		if (!label_scan_open(dev)) {
			log_error("Error opening device %s for writing at %llu length %u.",
				  dev_name(dev), (unsigned long long)start, (uint32_t)len);
			return false;
		}
	}

	if (!bcache_write_bytes(scan_bcache, dev->bcache_fd, start, len, data)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		dev_unset_last_byte(dev);
		label_scan_invalidate(dev);
		return false;
	}

	if (!bcache_flush(scan_bcache)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		dev_unset_last_byte(dev);
		label_scan_invalidate(dev);
		return false;
	}
	return true;
}

bool dev_invalidate_bytes(struct device *dev, uint64_t start, size_t len)
{
	return bcache_invalidate_bytes(scan_bcache, dev->bcache_fd, start, len);
}

bool dev_write_zeros(struct device *dev, uint64_t start, size_t len)
{
	if (test_mode())
		return true;

	if (!scan_bcache) {
		log_error("dev_write_zeros bcache not set up %s", dev_name(dev));
		return false;
	}

	if (_in_bcache(dev) && !(dev->flags & DEV_BCACHE_WRITE)) {
		/* FIXME: avoid tossing out bcache blocks just to replace fd. */
		log_debug("Close and reopen to write %s", dev_name(dev));
		_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);

		dev->flags |= DEV_BCACHE_WRITE;
		label_scan_open(dev);
	}

	if (dev->bcache_fd <= 0) {
		/* This is not often needed. */
		dev->flags |= DEV_BCACHE_WRITE;
		if (!label_scan_open(dev)) {
			log_error("Error opening device %s for writing at %llu length %u.",
				  dev_name(dev), (unsigned long long)start, (uint32_t)len);
			return false;
		}
	}

	dev_set_last_byte(dev, start + len);

	if (!bcache_zero_bytes(scan_bcache, dev->bcache_fd, start, len)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		dev_unset_last_byte(dev);
		label_scan_invalidate(dev);
		return false;
	}

	if (!bcache_flush(scan_bcache)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		dev_unset_last_byte(dev);
		label_scan_invalidate(dev);
		return false;
	}
	dev_unset_last_byte(dev);
	return true;
}

bool dev_set_bytes(struct device *dev, uint64_t start, size_t len, uint8_t val)
{
	if (test_mode())
		return true;

	if (!scan_bcache) {
		log_error("dev_set_bytes bcache not set up %s", dev_name(dev));
		return false;
	}

	if (_in_bcache(dev) && !(dev->flags & DEV_BCACHE_WRITE)) {
		/* FIXME: avoid tossing out bcache blocks just to replace fd. */
		log_debug("Close and reopen to write %s", dev_name(dev));
		_invalidate_fd(scan_bcache, dev->bcache_fd);
		_scan_dev_close(dev);
		/* goes to label_scan_open() since bcache_fd < 0 */
	}

	if (dev->bcache_fd <= 0) {
		/* This is not often needed. */
		dev->flags |= DEV_BCACHE_WRITE;
		if (!label_scan_open(dev)) {
			log_error("Error opening device %s for writing at %llu length %u.",
				  dev_name(dev), (unsigned long long)start, (uint32_t)len);
			return false;
		}
	}

	dev_set_last_byte(dev, start + len);

	if (!bcache_set_bytes(scan_bcache, dev->bcache_fd, start, len, val)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		dev_unset_last_byte(dev);
		label_scan_invalidate(dev);
		return false;
	}

	if (!bcache_flush(scan_bcache)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		dev_unset_last_byte(dev);
		label_scan_invalidate(dev);
		return false;
	}

	dev_unset_last_byte(dev);
	return true;
}

void dev_set_last_byte(struct device *dev, uint64_t offset)
{
	unsigned int physical_block_size = 0;
	unsigned int logical_block_size = 0;
	unsigned int bs;

	if (!dev_get_direct_block_sizes(dev, &physical_block_size, &logical_block_size)) {
		stack;
		return; /* FIXME: error path ? */
	}

	if ((physical_block_size == 512) && (logical_block_size == 512))
		bs = 512;
	else if ((physical_block_size == 4096) && (logical_block_size == 4096))
		bs = 4096;
	else if ((physical_block_size == 512) || (logical_block_size == 512)) {
		log_debug("Set last byte mixed block sizes physical %u logical %u using 512",
			  physical_block_size, logical_block_size);
		bs = 512;
	} else if ((physical_block_size == 4096) || (logical_block_size == 4096)) {
		log_debug("Set last byte mixed block sizes physical %u logical %u using 4096",
			  physical_block_size, logical_block_size);
		bs = 4096;
	} else {
		log_debug("Set last byte mixed block sizes physical %u logical %u using 512",
			  physical_block_size, logical_block_size);
		bs = 512;
	}

	bcache_set_last_byte(scan_bcache, dev->bcache_fd, offset, bs);
}

void dev_unset_last_byte(struct device *dev)
{
	bcache_unset_last_byte(scan_bcache, dev->bcache_fd);
}

