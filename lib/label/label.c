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

#include "lib/misc/lib.h"
#include "base/memory/zalloc.h"
#include "lib/label/label.h"
#include "lib/misc/crc.h"
#include "lib/mm/xlate.h"
#include "lib/cache/lvmcache.h"
#include "lib/device/bcache.h"
#include "lib/commands/toolcontext.h"
#include "lib/activate/activate.h"
#include "lib/label/hints.h"
#include "lib/metadata/metadata.h"
#include "lib/format_text/layout.h"
#include "lib/device/device_id.h"
#include "lib/device/online.h"

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
				   char *headers_buf,
				   size_t headers_buf_size,
				   uint64_t *label_sector,
				   uint64_t block_sector,
				   uint64_t start_sector)
{
	struct labeller_i *li;
	struct labeller *labeller_ret = NULL;
	struct label_header *lh;
	uint64_t sector;
	int found = 0;

	for (sector = start_sector; sector < start_sector + LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {

		if ((sector * 512) >= headers_buf_size)
			break;

		lh = (struct label_header *) (headers_buf + (sector << SECTOR_SHIFT));

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
				log_debug("Found label at sector %llu on %s",
					  (unsigned long long)(block_sector + sector), dev_name(dev));
				if (found) {
					log_error("Ignoring additional label on %s at sector %llu",
						  dev_name(dev),
						  (unsigned long long)(block_sector + sector));
					continue;
				}

				labeller_ret = li->l;
				found = 1;

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
			  struct device *dev, char *headers_buf, size_t headers_buf_size,
			  uint64_t block_sector, uint64_t start_sector,
			  int *is_lvm_device)
{
	char *label_buf;
	struct labeller *labeller;
	uint64_t label_sector = 0;
	int is_duplicate = 0;
	int ret = 0;

	dev->flags &= ~DEV_SCAN_FOUND_LABEL;

	/*
	 * The device may have signatures that exclude it from being processed,
	 * even if it might look like a PV.  Now that the device has been read
	 * and data is available in bcache for it, recheck filters, including
	 * those that use data.  The device needs to be excluded before it
	 * begins to be processed as a PV.
	 */
	if (f) {
		if (!f->passes_filter(cmd, f, dev, NULL)) {
			/*
			 * If this device was previously scanned (not common)
			 * and if it passed filters at that point, lvmcache
			 * info may have been saved for it.  Now the same
			 * device is being scanned again, and it may fail
			 * filters this time.  If the caller did not clear
			 * lvmcache info for this dev before rescanning, do
			 * that now.  It's unlikely this is actually needed.
			 */
			if (dev->pvid[0]) {
				log_print_unless_silent("Clear pvid and info for filtered dev %s.", dev_name(dev));
				lvmcache_del_dev(dev);
				memset(dev->pvid, 0, sizeof(dev->pvid));
			}

			*is_lvm_device = 0;
			goto_out;
		}
	}

	/*
	 * Finds the data sector containing the label.
	 */
	if (!(labeller = _find_lvm_header(dev, headers_buf, headers_buf_size, &label_sector, block_sector, start_sector))) {

		/*
		 * Non-PVs exit here
		 *
		 * FIXME: check for PVs with errors that also exit here!
		 * i.e. this code cannot distinguish between a non-lvm
		 * device an an lvm device with errors.
		 */

		log_very_verbose("%s: No lvm label detected", dev_name(dev));

		/* See comment above */
		if (dev->pvid[0]) {
			log_print_unless_silent("Clear pvid and info for no lvm header %s", dev_name(dev));
			lvmcache_del_dev(dev);
			memset(dev->pvid, 0, sizeof(dev->pvid));
		}

		dev->flags |= DEV_SCAN_FOUND_NOLABEL;
		*is_lvm_device = 0;
		goto out;
	}

	dev->flags |= DEV_SCAN_FOUND_LABEL;
	*is_lvm_device = 1;
	label_buf = headers_buf + (label_sector * 512);

	/*
	 * This is the point where the scanning code dives into the rest of
	 * lvm.  ops->read() is _text_read() which reads the pv_header, mda
	 * locations, and metadata text.  All of the info it finds about the PV
	 * and VG is stashed in lvmcache which saves it in the form of
	 * info/vginfo structs.  That lvmcache info is used later when the
	 * command wants to read the VG to do something to it.
	 */
	ret = labeller->ops->read(cmd, labeller, dev, label_buf, label_sector, &is_duplicate);

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
	const char *modestr;
	struct stat sbuf;
	int flags = 0;
	int fd, di;

	if (!dev)
		return 0;

	if (dev->flags & DEV_IN_BCACHE) {
		/* Shouldn't happen */
		log_error("Device open %s has DEV_IN_BCACHE already set", dev_name(dev));
		dev->flags &= ~DEV_IN_BCACHE;
	}

	if (dev->bcache_di != -1) {
		/* Shouldn't happen */
		log_error("Device open %s already open with di %d fd %d",
			  dev_name(dev), dev->bcache_di, dev->bcache_fd);
		return 0;
	}

 next_name:
	/*
	 * All the names for this device (major:minor) are kept on
	 * dev->aliases, the first one is the primary/preferred name.
	 *
	 * The default name preferences in dev-cache mean that the first
	 * name in dev->aliases is not a symlink for scsi devices, but is
	 * the /dev/mapper/ symlink for mpath devices.
	 *
	 * If preferred names are set to symlinks, should this
	 * first attempt to open using a non-symlink?
	 *
	 * dm_list_first() returns NULL if the list is empty.
	 */
	if (!(name_list = dm_list_first(&dev->aliases))) {
		log_error("Device open %d:%d has no path names.",
			  (int)MAJOR(dev->dev), (int)MINOR(dev->dev));
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
		modestr = "rwex";
	} else if (dev->flags & DEV_BCACHE_WRITE) {
		flags |= O_RDWR;
		modestr = "rw";
	} else {
		flags |= O_RDONLY;
		modestr = "ro";
	}

	fd = open(name, flags, 0777);
	if (fd < 0) {
		if ((errno == EBUSY) && (flags & O_EXCL)) {
			log_error("Can't open %s exclusively.  Mounted filesystem?",
				  dev_name(dev));
			return 0;
		} else {
			/*
			 * drop name from dev->aliases and use verify_aliases to
			 * drop any other invalid aliases before retrying open with
			 * any remaining valid paths.
			 */
			log_debug("Drop alias for %d:%d failed open %s (%d)",
				  (int)MAJOR(dev->dev), (int)MINOR(dev->dev), name, errno);
			dev_cache_failed_path(dev, name);
			dev_cache_verify_aliases(dev);
			goto next_name;
		}
	}

	/* Verify that major:minor from the path still match dev. */
	if ((fstat(fd, &sbuf) < 0) || (sbuf.st_rdev != dev->dev)) {
		log_warn("Invalid path %s for device %d:%d, trying different path.",
			 name, (int)MAJOR(dev->dev), (int)MINOR(dev->dev));
		(void)close(fd);
		dev_cache_failed_path(dev, name);
		dev_cache_verify_aliases(dev);
		goto next_name;
	}

	dev->flags |= DEV_IN_BCACHE;
	dev->bcache_fd = fd;

	di = bcache_set_fd(fd);

	if (di == -1) {
		log_error("Failed to set bcache fd.");
		if (close(fd))
			log_sys_debug("close", name);
		dev->bcache_fd = -1;
		return 0;
	}

	log_debug("open %s %s di %d fd %d", dev_name(dev), modestr, di, fd);

	dev->bcache_di = di;

	return 1;
}

static int _scan_dev_close(struct device *dev)
{
	if (!(dev->flags & DEV_IN_BCACHE))
		log_error("scan_dev_close %s no DEV_IN_BCACHE set", dev_name(dev));

	dev->flags &= ~DEV_IN_BCACHE;
	dev->flags &= ~DEV_BCACHE_EXCL;
	dev->flags &= ~DEV_BCACHE_WRITE;

	if (dev->bcache_di == -1) {
		log_error("scan_dev_close %s already closed", dev_name(dev));
		return 0;
	}

	bcache_clear_fd(dev->bcache_di);

	if (close(dev->bcache_fd))
		log_warn("close %s errno %d", dev_name(dev), errno);

	dev->bcache_fd = -1;
	dev->bcache_di = -1;

	return 1;
}

// Like bcache_invalidate, only it throws any dirty data away if the
// write fails.
static void _invalidate_di(struct bcache *cache, int di)
{
	if (!bcache_invalidate_di(cache, di))
		bcache_abort_di(cache, di);
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

#define HEADERS_BUF_SIZE 4096

static int _scan_list(struct cmd_context *cmd, struct dev_filter *f,
		      struct dm_list *devs, int want_other_devs, int *failed)
{
	char headers_buf[HEADERS_BUF_SIZE];
	struct dm_list wait_devs;
	struct dm_list done_devs;
	struct device_list *devl, *devl2;
	struct block *bb;
	int scan_read_errors = 0;
	int scan_process_errors = 0;
	int scan_failed_count = 0;
	int rem_prefetches;
	int submit_count;
	int is_lvm_device;
	int ret;

	dm_list_init(&wait_devs);
	dm_list_init(&done_devs);

	log_debug_devs("Scanning %d devices for VG info", dm_list_size(devs));

 scan_more:
	rem_prefetches = bcache_max_prefetches(scan_bcache);
	submit_count = 0;

	dm_list_iterate_items_safe(devl, devl2, devs) {

		devl->dev->flags &= ~DEV_SCAN_NOT_READ;

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
				log_debug_devs("Scan failed to open %d:%d %s.",
					       (int)MAJOR(devl->dev->dev), (int)MINOR(devl->dev->dev), dev_name(devl->dev));
				dm_list_del(&devl->list);
				devl->dev->flags |= DEV_SCAN_NOT_READ;
				continue;
			}
		}

		bcache_prefetch(scan_bcache, devl->dev->bcache_di, 0);

		rem_prefetches--;
		submit_count++;

		dm_list_del(&devl->list);
		dm_list_add(&wait_devs, &devl->list);
	}

	log_debug_devs("Scanning submitted %d reads", submit_count);

	dm_list_iterate_items_safe(devl, devl2, &wait_devs) {
		bb = NULL;
		is_lvm_device = 0;

		if (!bcache_get(scan_bcache, devl->dev->bcache_di, 0, 0, &bb)) {
			log_debug_devs("Scan failed to read %s.", dev_name(devl->dev));
			scan_read_errors++;
			scan_failed_count++;
			devl->dev->flags |= DEV_SCAN_NOT_READ;
			lvmcache_del_dev(devl->dev);
			if (bb)
				bcache_put(bb);
		} else {
			/* copy the first 4k from bb that will contain label_header */

			memcpy(headers_buf, bb->data, HEADERS_BUF_SIZE);

			/*
			 * "put" the bcache block before process_block because
			 * processing metadata may need to invalidate and reread
			 * metadata that's covered by bb. invalidate/reread is
			 * not allowed while bb is held.  The functions for
			 * filtering and scanning metadata for this device use
			 * dev_read_bytes(), which will generally grab the
			 * bcache block/data that we're putting here.  Since
			 * we're doing put, it's possible but not likely that
			 * bcache could drop the block before dev_read_bytes()
			 * uses it again, in which case bcache will reread it
			 * from disk for dev_read_bytes().
			 */
			bcache_put(bb);

			log_debug_devs("Processing data from device %s %d:%d di %d",
				       dev_name(devl->dev),
				       (int)MAJOR(devl->dev->dev),
				       (int)MINOR(devl->dev->dev),
				       devl->dev->bcache_di);

			ret = _process_block(cmd, f, devl->dev, headers_buf, sizeof(headers_buf), 0, 0, &is_lvm_device);

			if (!ret && is_lvm_device) {
				log_debug_devs("Scan failed to process %s", dev_name(devl->dev));
				scan_process_errors++;
				scan_failed_count++;
			}
		}

		/*
		 * Keep the bcache block of lvm devices we have processed so
		 * that the vg_read phase can reuse it.  If bcache failed to
		 * read the block, or the device does not belong to lvm, then
		 * drop it from bcache.  When "want_other_devs" is set, it
		 * means the caller wants to scan and keep open non-lvm devs,
		 * e.g. to pvcreate them.
		 */
		if (!is_lvm_device && !want_other_devs) {
			_invalidate_di(scan_bcache, devl->dev->bcache_di);
			_scan_dev_close(devl->dev);
		}

		dm_list_del(&devl->list);
		dm_list_add(&done_devs, &devl->list);
	}

	if (!dm_list_empty(devs))
		goto scan_more;

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
		log_error("Failed to set up io layer with %d blocks.", cache_blocks);
		return 0;
	}

	return 1;
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

void prepare_open_file_limit(struct cmd_context *cmd, unsigned int num_devs)
{
#ifdef HAVE_PRLIMIT
	struct rlimit old = { 0 }, new;
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

/*
 * Currently the only caller is pvck which probably doesn't need
 * deferred filters checked after the read... it wants to know if
 * anything has the pvid, even a dev that might be filtered.
 */

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

	/*
	 * Creates a list of available devices, does not open or read any,
	 * and does not filter them.
	 */
	if (!setup_devices(cmd)) {
		log_error("Failed to set up devices.");
		return 0;
	}

	/*
	 * Iterating over all available devices with cmd->filter filters
	 * devices; those returned from dev_iter_get are the devs that
	 * pass filters, and are those we can use.
	 */

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
 * Clear state that label_scan_vg_online() created so it will not
 * confuse the standard label_scan() that the caller falls back to.
 * the results of filtering (call filter->wipe)
 * the results of matching device_id (reset dev and du)
 * the results of scanning in lvmcache
 */
static void _clear_scan_state(struct cmd_context *cmd, struct dm_list *devs)
{
	struct device_list *devl;
	struct device *dev;
	struct dev_use *du;

	dm_list_iterate_items(devl, devs) {
		dev = devl->dev;

		cmd->filter->wipe(cmd, cmd->filter, dev, NULL);
		dev->flags &= ~DEV_MATCHED_USE_ID;
		dev->id = NULL;

		if ((du = get_du_for_dev(cmd, dev)))
			du->dev = NULL;

		lvmcache_del_dev(dev);

		memset(dev->pvid, 0, ID_LEN);
	}
}

/*
 * Use files under /run/lvm/, created by pvscan --cache autoactivation,
 * to optimize device setup/scanning.  autoactivation happens during
 * system startup when the hints file is not useful, but he pvs_online
 * files can provide a similar optimization to the hints file.
 */
 
int label_scan_vg_online(struct cmd_context *cmd, const char *vgname,
			 int *found_none, int *found_all, int *found_incomplete)
{
	struct dm_list pvs_online;
	struct dm_list devs;
	struct dm_list devs_drop;
	struct pv_online *po;
	struct device_list *devl, *devl2;
	int relax_deviceid_filter = 0;
	unsigned metadata_pv_count;
	int try_dev_scan = 0;

	dm_list_init(&pvs_online);
	dm_list_init(&devs);
	dm_list_init(&devs_drop);

	log_debug_devs("Finding online devices to scan");

	/*
	 * First attempt to use /run/lvm/pvs_lookup/vgname which should be
	 * used in cases where all PVs in a VG do not contain metadata.
	 * When the pvs_lookup file does not exist, then simply use all
	 * /run/lvm/pvs_online/pvid files that contain a matching vgname.
	 * The list of po structs represents the PVs in the VG, and the
	 * info from the online files tell us which devices those PVs are
	 * located on.
	 */
	if (vgname) {
		if (!get_pvs_lookup(&pvs_online, vgname)) {
			if (!get_pvs_online(&pvs_online, vgname))
				goto bad;
		}
	} else {
		if (!get_pvs_online(&pvs_online, NULL))
			goto bad;
	}

	if (dm_list_empty(&pvs_online)) {
		*found_none = 1;
		return 1;
	}

	/*
	 * For each po add a struct dev to dev-cache.  This is a faster
	 * alternative to the usual dev_cache_scan() which looks at all
	 * devices.  If this optimization fails, then fall back to the usual
	 * dev_cache_scan().
	 */
	dm_list_iterate_items(po, &pvs_online) {
		if (!(po->dev = setup_dev_in_dev_cache(cmd, po->devno, po->devname[0] ? po->devname : NULL))) {
			log_debug("No device found for quick mapping of online PV %d:%d %s PVID %s",
				  (int)MAJOR(po->devno), (int)MINOR(po->devno), po->devname, po->pvid);
			try_dev_scan = 1;
			continue;
		}
		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			goto_bad;

		devl->dev = po->dev;
		dm_list_add(&devs, &devl->list);
	}

	/*
	 * Translating a devno (major:minor) into a device name can be
	 * problematic for some devices that have unusual sysfs layouts, so if
	 * this happens, do a full dev_cache_scan, which is slower, but is
	 * sure to find the device.
	 */
	if (try_dev_scan) {
		log_debug("Repeat dev cache scan to translate devnos.");
		dev_cache_scan(cmd);
		dm_list_iterate_items(po, &pvs_online) {
			if (po->dev)
				continue;
			if (!(po->dev = dev_cache_get_by_devt(cmd, po->devno))) {
				log_error("No device found for %d:%d PVID %s",
					  (int)MAJOR(po->devno), (int)MINOR(po->devno), po->pvid);
				goto bad;
			}
			if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
				goto_bad;

			devl->dev = po->dev;
			dm_list_add(&devs, &devl->list);
		}
	}

	/*
	 * factor code common to pvscan_cache_args
	 */

	/*
	 * Match devs with the devices file because special/optimized
	 * device setup was used which does not check the devices file.
	 * If a match fails here do not exclude it, that will be done below by
	 * passes_filter() which runs filter-deviceid. The
	 * relax_deviceid_filter case needs to be able to work around
	 * unmatching devs.
	 */

	if (cmd->enable_devices_file) {
		dm_list_iterate_items(devl, &devs)
			device_ids_match_dev(cmd, devl->dev);
	}

	if (cmd->enable_devices_list)
		device_ids_match_device_list(cmd);

	if (cmd->enable_devices_file && device_ids_use_devname(cmd)) {
		relax_deviceid_filter = 1;
		cmd->filter_deviceid_skip = 1;
		/* PVIDs read from devs matched to devices file below instead. */
		log_debug("Skipping device_id filtering due to devname ids.");
	}

	/*
	 * See corresponding code in pvscan.  This function is used during
	 * startup autoactivation when udev has not created all symlinks, so
	 * regex filter containing symlinks doesn't work.  pvscan has code
	 * to properly check devs against the filter using DEVLINKS.  The
	 * pvscan will only create pvs_online files for devs that pass the
	 * filter.  We get devs from the pvs_online files, so we inherit the
	 * regex filtering from pvscan and don't have to do it ourself.
	 */
	cmd->filter_regex_skip = 1;

	cmd->filter_nodata_only = 1;

	dm_list_iterate_items_safe(devl, devl2, &devs) {
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL)) {
			log_print_unless_silent("%s excluded: %s.",
						dev_name(devl->dev), dev_filtered_reason(devl->dev));
			dm_list_del(&devl->list);
			dm_list_add(&devs_drop, &devl->list);
		}
	}

	cmd->filter_nodata_only = 0;

	/*
	 * Clear the results of nodata filters that were saved by the
	 * persistent filter so that the complete set of filters will
	 * be checked by passes_filter below.
	 */
	dm_list_iterate_items(devl, &devs)
		cmd->filter->wipe(cmd, cmd->filter, devl->dev, NULL);

	/*
	 * Read header from each dev.
	 * Eliminate non-lvm devs.
	 * Apply all filters.
	 */

	log_debug("label_scan_vg_online: read and filter devs");

	label_scan_setup_bcache();

	dm_list_iterate_items_safe(devl, devl2, &devs) {
		struct dev_use *du;
		int has_pvid;

		if (!label_read_pvid(devl->dev, &has_pvid)) {
			log_print_unless_silent("%s cannot read label.", dev_name(devl->dev));
			dm_list_del(&devl->list);
			dm_list_add(&devs_drop, &devl->list);
			continue;
		}

		if (!has_pvid) {
			/* Not an lvm device */
			log_print_unless_silent("%s not an lvm device.", dev_name(devl->dev));
			dm_list_del(&devl->list);
			dm_list_add(&devs_drop, &devl->list);
			continue;
		}

		/*
		 * filter-deviceid is not being used because of unstable devnames,
		 * so in place of that check if the pvid is in the devices file.
		 */
		if (relax_deviceid_filter) {
			if (!(du = get_du_for_pvid(cmd, devl->dev->pvid))) {
				log_print_unless_silent("%s excluded by devices file (checking PVID).",
							dev_name(devl->dev));
				dm_list_del(&devl->list);
				dm_list_add(&devs_drop, &devl->list);
				continue;
			} else {
				/* Special case matching for devname entries based on pvid. */
				log_debug("Match device_id %s %s to %s: matching PVID",
					  idtype_to_str(du->idtype), du->idname, dev_name(devl->dev));
			}
		}

		/* Applies all filters, including those that need data from dev. */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL)) {
			log_print_unless_silent("%s excluded: %s.",
						dev_name(devl->dev), dev_filtered_reason(devl->dev));
			dm_list_del(&devl->list);
			dm_list_add(&devs_drop, &devl->list);
		}
	}

	if (relax_deviceid_filter)
		cmd->filter_deviceid_skip = 0;

	cmd->filter_regex_skip = 0;

	free_po_list(&pvs_online);

	if (dm_list_empty(&devs)) {
		_clear_scan_state(cmd, &devs_drop);
		*found_none = 1;
		return 1;
	}

	/*
	 * Scan devs to populate lvmcache info, which includes the mda info that's
	 * needed to read vg metadata.
	 * bcache data from label_read_pvid above is not invalidated so it can
	 * be reused (more data may need to be read depending on how much of the
	 * metadata was covered when reading the pvid.)
	 */
	_scan_list(cmd, NULL, &devs, 0, NULL);

	/*
	 * Check if all PVs from the VG were found after scanning the devs
	 * produced from the online files.  The online files are effectively
	 * hints that usually work, but are not definitive, so we need to
	 * be able to fall back to a standard label scan if the online hints
	 * gave fewer PVs than listed in VG metadata.
	 */
	if (vgname) {
		metadata_pv_count = lvmcache_pvsummary_count(vgname);
		if (metadata_pv_count > dm_list_size(&devs)) {
			log_debug("Incomplete PV list from online files %d metadata %d.",
				  dm_list_size(&devs), metadata_pv_count);
			_clear_scan_state(cmd, &devs_drop);
			_clear_scan_state(cmd, &devs);
			*found_incomplete = 1;
			return 1;
		}
	}

	*found_all = 1;
	return 1;
bad:
	_clear_scan_state(cmd, &devs_drop);
	_clear_scan_state(cmd, &devs);
	free_po_list(&pvs_online);
	return 0;
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
	struct dm_list filtered_devs;
	struct dm_list scan_devs;
	struct dm_list hints_list;
	struct dev_iter *iter;
	struct device_list *devl, *devl2;
	struct device *dev;
	uint64_t max_metadata_size_bytes;
	int device_ids_invalid = 0;
	int using_hints;
	int create_hints = 0; /* NEWHINTS_NONE */

	log_debug_devs("Finding devices to scan");

	dm_list_init(&all_devs);
	dm_list_init(&filtered_devs);
	dm_list_init(&scan_devs);
	dm_list_init(&hints_list);

	if (!scan_bcache) {
		if (!_setup_bcache())
			return_0;
	}

	/*
	 * Creates a list of available devices, does not open or read any,
	 * and does not filter them.  The list of all available devices
	 * is kept in "dev-cache", and comes from /dev entries or libudev.
	 * The list of devs found here needs to be filtered to get the
	 * list of devs we can use. The dev_iter calls using cmd->filter
	 * are what filters the devs.
	 */
	if (!setup_devices(cmd)) {
		log_error("Failed to set up devices.");
		return 0;
	}

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
	 * Create a list of all devices in dev-cache (all found on the system.)
	 * Do not apply filters and do not read any (the filter arg is NULL).
	 * Invalidate bcache data for all devs (there will usually be no bcache
	 * data to invalidate.)
	 */
	if (!(iter = dev_iter_create(NULL, 0))) {
		log_error("Failed to get device list.");
		return 0;
	}
	while ((dev = dev_iter_get(cmd, iter))) {
		if (!(devl = zalloc(sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(&all_devs, &devl->list);

		/*
		 * label_scan should not generally be called a second time,
		 * so this will usually do nothing.
		 */
		label_scan_invalidate(dev);
	}
	dev_iter_destroy(iter);

	/*
	 * Exclude devices that fail nodata filters. (Those filters that can be
	 * checked without reading data from the device.)
	 *
	 * The result of checking nodata filters is saved by the "persistent
	 * filter", and this result needs to be cleared (wiped) so that the
	 * complete set of filters (including those that require data) can be
	 * checked in _process_block, where headers have been read.
	 *
	 * FIXME: devs that are filtered with data in _process_block
	 * are not moved to the filtered_devs list like devs filtered
	 * here without data.  Does that have any effect?
	 */
	log_debug_devs("Filtering devices to scan (nodata)");

	cmd->filter_nodata_only = 1;
	dm_list_iterate_items_safe(devl, devl2, &all_devs) {
		dev = devl->dev;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			dm_list_del(&devl->list);
			dm_list_add(&filtered_devs, &devl->list);

			if (dev->pvid[0]) {
				log_print_unless_silent("Clear pvid and info for filtered dev %s.",
							dev_name(dev));
				lvmcache_del_dev(dev);
				memset(dev->pvid, 0, sizeof(dev->pvid));
			}
		}
	}

	log_debug_devs("Filtering devices to scan done (nodata)");

	cmd->filter_nodata_only = 0;

	dm_list_iterate_items(devl, &all_devs)
		cmd->filter->wipe(cmd, cmd->filter, devl->dev, NULL);
	dm_list_iterate_items(devl, &filtered_devs)
		cmd->filter->wipe(cmd, cmd->filter, devl->dev, NULL);

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

	/*
	 * If the total number of devices exceeds the soft open file
	 * limit, then increase the soft limit to the hard/max limit
	 * in case the number of PVs in scan_devs (it's only the PVs
	 * which we want to keep open) is higher than the current
	 * soft limit.
	 */
	prepare_open_file_limit(cmd, dm_list_size(&scan_devs));

	/*
	 * Do the main scan.
	 */
	_scan_list(cmd, cmd->filter, &scan_devs, 0, NULL);

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

	/*
	 * If we're using hints to limit which devs we scanned, verify
	 * that those hints were valid, and if not we need to scan the
	 * rest of the devs.
	 */
	if (using_hints) {
		if (!validate_hints(cmd, &hints_list)) {
			log_debug("Will scan %d remaining devices", dm_list_size(&all_devs));
			_scan_list(cmd, cmd->filter, &all_devs, 0, NULL);
			/* scan_devs are the devs that have been scanned */
			dm_list_splice(&scan_devs, &all_devs);
			using_hints = 0;
			create_hints = 0;
			/* invalid hints means a new dev probably appeared and
			   we should search for any missing pvids again. */
			unlink_searched_devnames(cmd);
		}
	}

	free_hints(&hints_list);

	/*
	 * Check if the devices_file content is up to date and
	 * if not update it.
	 */
	device_ids_validate(cmd, &scan_devs, &device_ids_invalid, 0);

	dm_list_iterate_items_safe(devl, devl2, &all_devs) {
		dm_list_del(&devl->list);
		free(devl);
	}

	dm_list_iterate_items_safe(devl, devl2, &scan_devs) {
		dm_list_del(&devl->list);
		free(devl);
	}

	dm_list_iterate_items_safe(devl, devl2, &filtered_devs) {
		dm_list_del(&devl->list);
		free(devl);
	}

	/*
	 * Look for md components that might have been missed by filter-md
	 * during the scan.  With the label scanning complete we have metadata
	 * available that can sometimes offer a clue that a dev is actually an
	 * md component (device name hint, pv size vs dev size).  In some of
	 * those cases we may want to do a full md check on a dev that has been
	 * scanned.  This is done before hints are written so that any devs
	 * dropped due to being md components will not be included in a new
	 * hint file.
	 */
	lvmcache_extra_md_component_checks(cmd);

	/*
	 * If hints were not available/usable, then we scanned all devs,
	 * and we now know which are PVs.  Save this list of PVs we've
	 * identified as hints for the next command to use.
	 * (create_hints variable has NEWHINTS_X value which indicates
	 * the reason for creating the new hints.)
	 */
	if (create_hints && !device_ids_invalid)
		write_hint_file(cmd, create_hints);

	return 1;
}

/*
 * Read the header of the disk and if it's a PV
 * save the pvid in dev->pvid.
 */
int label_read_pvid(struct device *dev, int *has_pvid)
{
	char buf[4096] __attribute__((aligned(8)));
	struct label_header *lh;
	struct pv_header *pvh;

	memset(buf, 0, sizeof(buf));

	if (!label_scan_open(dev))
		return_0;

	/*
	 * We could do:
	 * dev_read_bytes(dev, 512, LABEL_SIZE, buf);
	 * which works, but there's a bcache issue that
	 * prevents proper invalidation after that.
	 */
	if (!dev_read_bytes(dev, 0, 4096, buf)) {
		label_scan_invalidate(dev);
		return_0;
	}

	if (has_pvid)
		*has_pvid = 0;

	lh = (struct label_header *)(buf + 512);
	if (memcmp(lh->id, LABEL_ID, sizeof(lh->id))) {
		/* Not an lvm device */
		label_scan_invalidate(dev);
		return 1;
	}

	/*
	 * wipefs -a just clears the type field, leaving the
	 * rest of the label_header intact.
	 */
	if (memcmp(lh->type, LVM2_LABEL, sizeof(lh->type))) {
		/* Not an lvm device */
		label_scan_invalidate(dev);
		return 1;
	}

	if (has_pvid)
		*has_pvid = 1;

	pvh = (struct pv_header *)(buf + 512 + 32);
	memcpy(dev->pvid, pvh->pv_uuid, ID_LEN);
	return 1;
}

/*
 * label_scan_devs without invalidating data for the devs first,
 * when the caller wants to make use of any bcache data that
 * they may have already read.
 */
int label_scan_devs_cached(struct cmd_context *cmd, struct dev_filter *f, struct dm_list *devs)
{
	if (!scan_bcache)
		return 0;

	_scan_list(cmd, f, devs, 0, NULL);

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
		if (_in_bcache(devl->dev))
			_invalidate_di(scan_bcache, devl->dev->bcache_di);
	}

	_scan_list(cmd, f, devs, 0, NULL);

	return 1;
}

int label_scan_devs_rw(struct cmd_context *cmd, struct dev_filter *f, struct dm_list *devs)
{
	struct device_list *devl;

	if (!scan_bcache) {
		if (!_setup_bcache())
			return 0;
	}

	dm_list_iterate_items(devl, devs) {
		if (_in_bcache(devl->dev))
			_invalidate_di(scan_bcache, devl->dev->bcache_di);
		devl->dev->flags |= DEV_BCACHE_WRITE;
	}

	_scan_list(cmd, f, devs, 0, NULL);

	return 1;
}

int label_scan_devs_excl(struct cmd_context *cmd, struct dev_filter *f, struct dm_list *devs)
{
	struct device_list *devl;
	int failed = 0;

	dm_list_iterate_items(devl, devs) {
		label_scan_invalidate(devl->dev);
		/*
		 * With this flag set, _scan_dev_open() done by
		 * _scan_list() will do open EXCL
		 */
		devl->dev->flags |= DEV_BCACHE_EXCL;
		devl->dev->flags |= DEV_BCACHE_WRITE;
	}

	_scan_list(cmd, f, devs, 1, &failed);

	if (failed)
		return 0;
	return 1;
}

void label_scan_invalidate(struct device *dev)
{
	if (_in_bcache(dev)) {
		_invalidate_di(scan_bcache, dev->bcache_di);
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

	if (lv_info(cmd, lv, 0, &lvinfo, 0, 0) && lvinfo.exists) {
		/* FIXME: Still unclear what is it supposed to find */
		devt = MKDEV(lvinfo.major, lvinfo.minor);
		if ((dev = dev_cache_get_by_devt(cmd, devt)))
			label_scan_invalidate(dev);
	}
}

void label_scan_invalidate_lvs(struct cmd_context *cmd, struct dm_list *lvs)
{
	struct dm_list *devs;
	struct dm_active_device *dm_dev;
	unsigned devs_features = 0;
	struct device *dev;
	struct lv_list *lvl;
	dev_t devt;

	/*
	 * This is only needed when the command sees PVs stacked on LVs which
	 * will only happen with scan_lvs=1.
	 */
	if (!cmd->scan_lvs)
		return;
	log_debug("invalidating devs for any pvs on lvs");

	if (get_device_list(NULL, &devs, &devs_features)) {
		if (devs_features & DM_DEVICE_LIST_HAS_UUID) {
			dm_list_iterate_items(dm_dev, devs)
				if (dm_dev->uuid &&
				    strncmp(dm_dev->uuid, UUID_PREFIX, sizeof(UUID_PREFIX) - 1) == 0) {
					devt = MKDEV(dm_dev->major, dm_dev->minor);
					if ((dev = dev_cache_get_by_devt(cmd, devt)))
						label_scan_invalidate(dev);
				}
			/* ATM no further caching for any lvconvert command
			 * TODO: any other command to be skipped ??
			 */
			if (strcmp(cmd->name, "lvconvert")) {
				dm_device_list_destroy(&cmd->cache_dm_devs);
				cmd->cache_dm_devs = devs; /* cache to avoid unneeded checks */
				devs = NULL;
			}
		}
		dm_device_list_destroy(&devs);
	}

	if (!(devs_features & DM_DEVICE_LIST_HAS_UUID))
		dm_list_iterate_items(lvl, lvs)
			label_scan_invalidate_lv(cmd, lvl->lv);
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
		cmd->filter->wipe(cmd, cmd->filter, dev, NULL);
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

int label_scan_dev(struct cmd_context *cmd, struct device *dev)
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

	label_scan_invalidate(dev);

	_scan_list(cmd, NULL, &one_dev, 0, &failed);

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
 * to be open so we can use dev->bcache_di to write.
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
		log_debug("close and reopen excl %s", dev_name(dev));
		_invalidate_di(scan_bcache, dev->bcache_di);
		_scan_dev_close(dev);
	}
	dev->flags |= DEV_BCACHE_EXCL;
	dev->flags |= DEV_BCACHE_WRITE;
	return label_scan_open(dev);
}

int label_scan_open_rw(struct device *dev)
{
	if (_in_bcache(dev) && !(dev->flags & DEV_BCACHE_WRITE)) {
		log_debug("close and reopen rw %s", dev_name(dev));
		_invalidate_di(scan_bcache, dev->bcache_di);
		_scan_dev_close(dev);
	}
	dev->flags |= DEV_BCACHE_WRITE;
	return label_scan_open(dev);
}

int label_scan_reopen_rw(struct device *dev)
{
	const char *name;
	int flags = 0;
	int prev_fd = dev->bcache_fd;
	int fd;

	if (dm_list_empty(&dev->aliases)) {
		log_error("Cannot reopen rw device %d:%d with no valid paths di %d fd %d.",
			  (int)MAJOR(dev->dev), (int)MINOR(dev->dev), dev->bcache_di, dev->bcache_fd);
		return 0;
	}

	name = dev_name(dev);
	if (!name || name[0] != '/') {
		log_error("Cannot reopen rw device %d:%d with no valid name di %d fd %d.",
			  (int)MAJOR(dev->dev), (int)MINOR(dev->dev), dev->bcache_di, dev->bcache_fd);
		return 0;
	}

	if (!(dev->flags & DEV_IN_BCACHE)) {
		if ((dev->bcache_fd != -1) || (dev->bcache_di != -1)) {
			/* shouldn't happen */
			log_debug("Reopen writeable %s uncached fd %d di %d",
				  dev_name(dev), dev->bcache_fd, dev->bcache_di);
			return 0;
		}
		dev->flags |= DEV_BCACHE_WRITE;
		return _scan_dev_open(dev);
	}

	if ((dev->flags & DEV_BCACHE_WRITE))
		return 1;

	if (dev->bcache_fd == -1) {
		log_error("Failed to open writable %s index %d fd none",
			  dev_name(dev), dev->bcache_di);
		return 0;
	}
	if (dev->bcache_di == -1) {
		log_error("Failed to open writeable %s index none fd %d",
			  dev_name(dev), dev->bcache_fd);
		return 0;
	}

	flags |= O_DIRECT;
	flags |= O_NOATIME;
	flags |= O_RDWR;

	fd = open(name, flags, 0777);
	if (fd < 0) {
		log_error("Failed to open rw %s errno %d di %d fd %d.",
			  dev_name(dev), errno, dev->bcache_di, dev->bcache_fd);
		return 0;
	}

	if (!bcache_change_fd(dev->bcache_di, fd)) {
		log_error("Failed to change to rw fd %s di %d fd %d.",
			  dev_name(dev), dev->bcache_di, fd);
		if (close(fd))
			log_sys_debug("close", dev_name(dev));
		return 0;
	}

	if (close(dev->bcache_fd))
		log_debug("reopen writeable %s close prev errno %d di %d fd %d.",
			  dev_name(dev), errno, dev->bcache_di, dev->bcache_fd);

	dev->flags |= DEV_IN_BCACHE;
	dev->flags |= DEV_BCACHE_WRITE;
	dev->bcache_fd = fd;

	log_debug("reopen writable %s di %d prev %d fd %d",
		  dev_name(dev), dev->bcache_di, prev_fd, fd);

	return 1;
}

bool dev_read_bytes(struct device *dev, uint64_t start, size_t len, void *data)
{
	if (!scan_bcache) {
		/* Should not happen */
		log_error("dev_read bcache not set up %s", dev_name(dev));
		return false;
	}

	if (dev->bcache_di < 0) {
		/* This is not often needed. */
		if (!label_scan_open(dev)) {
			log_error("Error opening device %s for reading at %llu length %u.",
				  dev_name(dev), (unsigned long long)start, (uint32_t)len);
			return false;
		}
	}

	if (!bcache_read_bytes(scan_bcache, dev->bcache_di, start, len, data)) {
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
		log_debug("close and reopen to write %s", dev_name(dev));
		_invalidate_di(scan_bcache, dev->bcache_di);
		_scan_dev_close(dev);

		dev->flags |= DEV_BCACHE_WRITE;
		(void) label_scan_open(dev); /* checked later */
	}

	if (dev->bcache_di < 0) {
		/* This is not often needed. */
		dev->flags |= DEV_BCACHE_WRITE;
		if (!label_scan_open(dev)) {
			log_error("Error opening device %s for writing at %llu length %u.",
				  dev_name(dev), (unsigned long long)start, (uint32_t)len);
			return false;
		}
	}

	if (!bcache_write_bytes(scan_bcache, dev->bcache_di, start, len, data)) {
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
	return bcache_invalidate_bytes(scan_bcache, dev->bcache_di, start, len);
}

void dev_invalidate(struct device *dev)
{
	bcache_invalidate_di(scan_bcache, dev->bcache_di);
}

bool dev_write_zeros(struct device *dev, uint64_t start, size_t len)
{
	return dev_set_bytes(dev, start, len, 0);
}

bool dev_set_bytes(struct device *dev, uint64_t start, size_t len, uint8_t val)
{
	bool rv;

	if (test_mode())
		return true;

	if (!scan_bcache) {
		log_error("dev_set_bytes bcache not set up %s", dev_name(dev));
		return false;
	}

	if (_in_bcache(dev) && !(dev->flags & DEV_BCACHE_WRITE)) {
		log_debug("close and reopen to write %s", dev_name(dev));
		_invalidate_di(scan_bcache, dev->bcache_di);
		_scan_dev_close(dev);
		/* goes to label_scan_open() since bcache_di < 0 */
	}

	if (dev->bcache_di == -1) {
		/* This is not often needed. */
		dev->flags |= DEV_BCACHE_WRITE;
		if (!label_scan_open(dev)) {
			log_error("Error opening device %s for writing at %llu length %u.",
				  dev_name(dev), (unsigned long long)start, (uint32_t)len);
			return false;
		}
	}

	dev_set_last_byte(dev, start + len);

	if (!val)
		rv = bcache_zero_bytes(scan_bcache, dev->bcache_di, start, len);
	else
		rv = bcache_set_bytes(scan_bcache, dev->bcache_di, start, len, val);

	if (!rv) {
		log_error("Error writing device value %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		goto fail;
	}

	if (!bcache_flush(scan_bcache)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		goto fail;
	}

	dev_unset_last_byte(dev);
	return true;

fail:
	dev_unset_last_byte(dev);
	label_scan_invalidate(dev);
	return false;
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

	bcache_set_last_byte(scan_bcache, dev->bcache_di, offset, bs);
}

void dev_unset_last_byte(struct device *dev)
{
	bcache_unset_last_byte(scan_bcache, dev->bcache_di);
}
