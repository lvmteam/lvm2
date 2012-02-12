/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "format-text.h"
#include "import-export.h"
#include "device.h"
#include "lvm-file.h"
#include "config.h"
#include "display.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "uuid.h"
#include "layout.h"
#include "crc.h"
#include "xlate.h"
#include "label.h"
#include "memlock.h"
#include "lvmcache.h"

#include <unistd.h>
#include <sys/file.h>
#include <sys/param.h>
#include <limits.h>
#include <dirent.h>
#include <ctype.h>

static struct format_instance *_text_create_text_instance(const struct format_type *fmt,
							  const struct format_instance_ctx *fic);

struct text_fid_context {
	char *raw_metadata_buf;
	uint32_t raw_metadata_buf_size;
};

struct dir_list {
	struct dm_list list;
	char dir[0];
};

struct raw_list {
	struct dm_list list;
	struct device_area dev_area;
};

int rlocn_is_ignored(const struct raw_locn *rlocn)
{
	return (rlocn->flags & RAW_LOCN_IGNORED ? 1 : 0);
}

void rlocn_set_ignored(struct raw_locn *rlocn, unsigned mda_ignored)
{
	if (mda_ignored)
		rlocn->flags |= RAW_LOCN_IGNORED;
	else
		rlocn->flags &= ~RAW_LOCN_IGNORED;
}

/*
 * NOTE: Currently there can be only one vg per text file.
 */

static int _text_vg_setup(struct format_instance *fid __attribute__((unused)),
			  struct volume_group *vg)
{
	if (vg->extent_size & (vg->extent_size - 1)) {
		log_error("Extent size must be power of 2");
		return 0;
	}

	return 1;
}

static uint64_t _mda_free_sectors_raw(struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;

	return mdac->free_sectors;
}

static uint64_t _mda_total_sectors_raw(struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;

	return mdac->area.size >> SECTOR_SHIFT;
}

/*
 * Check if metadata area belongs to vg
 */
static int _mda_in_vg_raw(struct format_instance *fid __attribute__((unused)),
			     struct volume_group *vg, struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct pv_list *pvl;

	dm_list_iterate_items(pvl, &vg->pvs)
		if (pvl->pv->dev == mdac->area.dev)
			return 1;

	return 0;
}

static unsigned _mda_locns_match_raw(struct metadata_area *mda1,
				     struct metadata_area *mda2)
{
	struct mda_context *mda1c = (struct mda_context *) mda1->metadata_locn;
	struct mda_context *mda2c = (struct mda_context *) mda2->metadata_locn;

	if ((mda1c->area.dev == mda2c->area.dev) &&
	    (mda1c->area.start == mda2c->area.start) &&
	    (mda1c->area.size == mda2c->area.size))
		return 1;

	return 0;
}

static struct device *_mda_get_device_raw(struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	return mdac->area.dev;
}

/*
 * For circular region between region_start and region_start + region_size,
 * back up one SECTOR_SIZE from 'region_ptr' and return the value.
 * This allows reverse traversal through text metadata area to find old
 * metadata.
 *
 * Parameters:
 *   region_start: start of the region (bytes)
 *   region_size: size of the region (bytes)
 *   region_ptr: pointer within the region (bytes)
 *   NOTE: region_start <= region_ptr <= region_start + region_size
 */
static uint64_t _get_prev_sector_circular(uint64_t region_start,
					  uint64_t region_size,
					  uint64_t region_ptr)
{
	if (region_ptr >= region_start + SECTOR_SIZE)
		return region_ptr - SECTOR_SIZE;
	else
		return (region_start + region_size - SECTOR_SIZE);
}

/*
 * Analyze a metadata area for old metadata records in the circular buffer.
 * This function just looks through and makes a first pass at the data in
 * the sectors for particular things.
 * FIXME: do something with each metadata area (try to extract vg, write
 * raw data to file, etc)
 */
static int _pv_analyze_mda_raw (const struct format_type * fmt,
				struct metadata_area *mda)
{
	struct mda_header *mdah;
	struct raw_locn *rlocn;
	uint64_t area_start;
	uint64_t area_size;
	uint64_t prev_sector, prev_sector2;
	uint64_t latest_mrec_offset;
	uint64_t offset;
	uint64_t offset2;
	size_t size;
	size_t size2;
	char *buf=NULL;
	struct device_area *area;
	struct mda_context *mdac;
	int r=0;

	mdac = (struct mda_context *) mda->metadata_locn;

	log_print("Found text metadata area: offset=%" PRIu64 ", size=%"
		  PRIu64, mdac->area.start, mdac->area.size);
	area = &mdac->area;

	if (!dev_open_readonly(area->dev))
		return_0;

	if (!(mdah = raw_read_mda_header(fmt, area)))
		goto_out;

	rlocn = mdah->raw_locns;

	/*
	 * The device area includes the metadata header as well as the
	 * records, so remove the metadata header from the start and size
	 */
	area_start = area->start + MDA_HEADER_SIZE;
	area_size = area->size - MDA_HEADER_SIZE;
	latest_mrec_offset = rlocn->offset + area->start;

	/*
	 * Start searching at rlocn (point of live metadata) and go
	 * backwards.
	 */
	prev_sector = _get_prev_sector_circular(area_start, area_size,
					       latest_mrec_offset);
	offset = prev_sector;
	size = SECTOR_SIZE;
	offset2 = size2 = 0;

	while (prev_sector != latest_mrec_offset) {
		prev_sector2 = prev_sector;
		prev_sector = _get_prev_sector_circular(area_start, area_size,
							prev_sector);
		if (prev_sector > prev_sector2)
			goto_out;
		/*
		 * FIXME: for some reason, the whole metadata region from
		 * area->start to area->start+area->size is not used.
		 * Only ~32KB seems to contain valid metadata records
		 * (LVM2 format - format_text).  As a result, I end up with
		 * "dm_config_maybe_section" returning true when there's no valid
		 * metadata in a sector (sectors with all nulls).
		 */
		if (!(buf = dm_malloc(size + size2)))
			goto_out;

		if (!dev_read_circular(area->dev, offset, size,
				       offset2, size2, buf))
			goto_out;

		/*
		 * FIXME: We could add more sophisticated metadata detection
		 */
		if (dm_config_maybe_section(buf, size + size2)) {
			/* FIXME: Validate region, pull out timestamp?, etc */
			/* FIXME: Do something with this region */
			log_verbose ("Found LVM2 metadata record at "
				     "offset=%"PRIu64", size=%"PRIsize_t", "
				     "offset2=%"PRIu64" size2=%"PRIsize_t,
				     offset, size, offset2, size2);
			offset = prev_sector;
			size = SECTOR_SIZE;
			offset2 = size2 = 0;
		} else {
			/*
			 * Not a complete metadata record, assume we have
			 * metadata and just increase the size and offset.
			 * Start the second region if the previous sector is
			 * wrapping around towards the end of the disk.
			 */
			if (prev_sector > offset) {
				offset2 = prev_sector;
				size2 += SECTOR_SIZE;
			} else {
				offset = prev_sector;
				size += SECTOR_SIZE;
			}
		}
		dm_free(buf);
		buf = NULL;
	}

	r = 1;
 out:
	if (buf)
		dm_free(buf);
	if (!dev_close(area->dev))
		stack;
	return r;
}



static int _text_lv_setup(struct format_instance *fid __attribute__((unused)),
			  struct logical_volume *lv)
{
/******** FIXME Any LV size restriction?
	uint64_t max_size = UINT_MAX;

	if (lv->size > max_size) {
		char *dummy = display_size(max_size);
		log_error("logical volumes cannot be larger than %s", dummy);
		dm_free(dummy);
		return 0;
	}
*/

	if (!*lv->lvid.s && !lvid_create(&lv->lvid, &lv->vg->id)) {
		log_error("Random lvid creation failed for %s/%s.",
			  lv->vg->name, lv->name);
		return 0;
	}

	return 1;
}

static void _xlate_mdah(struct mda_header *mdah)
{
	struct raw_locn *rl;

	mdah->version = xlate32(mdah->version);
	mdah->start = xlate64(mdah->start);
	mdah->size = xlate64(mdah->size);

	rl = &mdah->raw_locns[0];
	while (rl->offset) {
		rl->checksum = xlate32(rl->checksum);
		rl->offset = xlate64(rl->offset);
		rl->size = xlate64(rl->size);
		rl++;
	}
}

struct mda_header *raw_read_mda_header(const struct format_type *fmt,
				       struct device_area *dev_area)
{
	struct mda_header *mdah;

	if (!(mdah = dm_pool_alloc(fmt->cmd->mem, MDA_HEADER_SIZE))) {
		log_error("struct mda_header allocation failed");
		return NULL;
	}

	if (!dev_read(dev_area->dev, dev_area->start, MDA_HEADER_SIZE, mdah))
		goto_bad;

	if (mdah->checksum_xl != xlate32(calc_crc(INITIAL_CRC, (uint8_t *)mdah->magic,
						  MDA_HEADER_SIZE -
						  sizeof(mdah->checksum_xl)))) {
		log_error("Incorrect metadata area header checksum on %s"
			  " at offset %"PRIu64, dev_name(dev_area->dev),
			  dev_area->start);
		goto bad;
	}

	_xlate_mdah(mdah);

	if (strncmp((char *)mdah->magic, FMTT_MAGIC, sizeof(mdah->magic))) {
		log_error("Wrong magic number in metadata area header on %s"
			  " at offset %"PRIu64, dev_name(dev_area->dev),
			  dev_area->start);
		goto bad;
	}

	if (mdah->version != FMTT_VERSION) {
		log_error("Incompatible metadata area header version: %d on %s"
			  " at offset %"PRIu64, mdah->version,
			  dev_name(dev_area->dev), dev_area->start);
		goto bad;
	}

	if (mdah->start != dev_area->start) {
		log_error("Incorrect start sector in metadata area header: %"
			  PRIu64" on %s at offset %"PRIu64, mdah->start,
			  dev_name(dev_area->dev), dev_area->start);
		goto bad;
	}

	return mdah;

bad:
	dm_pool_free(fmt->cmd->mem, mdah);
	return NULL;
}

static int _raw_write_mda_header(const struct format_type *fmt,
				 struct device *dev,
				 uint64_t start_byte, struct mda_header *mdah)
{
	strncpy((char *)mdah->magic, FMTT_MAGIC, sizeof(mdah->magic));
	mdah->version = FMTT_VERSION;
	mdah->start = start_byte;

	_xlate_mdah(mdah);
	mdah->checksum_xl = xlate32(calc_crc(INITIAL_CRC, (uint8_t *)mdah->magic,
					     MDA_HEADER_SIZE -
					     sizeof(mdah->checksum_xl)));

	if (!dev_write(dev, start_byte, MDA_HEADER_SIZE, mdah))
		return_0;

	return 1;
}

static struct raw_locn *_find_vg_rlocn(struct device_area *dev_area,
				       struct mda_header *mdah,
				       const char *vgname,
				       int *precommitted)
{
	size_t len;
	char vgnamebuf[NAME_LEN + 2] __attribute__((aligned(8)));
	struct raw_locn *rlocn, *rlocn_precommitted;
	struct lvmcache_info *info;

	rlocn = mdah->raw_locns;	/* Slot 0 */
	rlocn_precommitted = rlocn + 1;	/* Slot 1 */

	/* Should we use precommitted metadata? */
	if (*precommitted && rlocn_precommitted->size &&
	    (rlocn_precommitted->offset != rlocn->offset)) {
		rlocn = rlocn_precommitted;
	} else
		*precommitted = 0;

	/* Do not check non-existent metadata. */
	if (!rlocn->offset && !rlocn->size)
		return NULL;

	/*
	 * Don't try to check existing metadata
	 * if given vgname is an empty string.
	 */
	if (!*vgname)
		return rlocn;

	/* FIXME Loop through rlocns two-at-a-time.  List null-terminated. */
	/* FIXME Ignore if checksum incorrect!!! */
	if (!dev_read(dev_area->dev, dev_area->start + rlocn->offset,
		      sizeof(vgnamebuf), vgnamebuf))
		goto_bad;

	if (!strncmp(vgnamebuf, vgname, len = strlen(vgname)) &&
	    (isspace(vgnamebuf[len]) || vgnamebuf[len] == '{'))
		return rlocn;
	else
		log_debug("Volume group name found in metadata does "
			  "not match expected name %s.", vgname);

      bad:
	if ((info = lvmcache_info_from_pvid(dev_area->dev->pvid, 0)))
		lvmcache_update_vgname_and_id(info, FMT_TEXT_ORPHAN_VG_NAME,
					      FMT_TEXT_ORPHAN_VG_NAME, 0, NULL);

	return NULL;
}

/*
 * Determine offset for uncommitted metadata
 */
static uint64_t _next_rlocn_offset(struct raw_locn *rlocn,
				   struct mda_header *mdah)
{
	if (!rlocn)
		/* Find an empty slot */
		/* FIXME Assume only one VG per mdah for now */
		return MDA_HEADER_SIZE;

	/* Start of free space - round up to next sector; circular */
	return ((rlocn->offset + rlocn->size +
		(SECTOR_SIZE - rlocn->size % SECTOR_SIZE) -
		MDA_HEADER_SIZE) % (mdah->size - MDA_HEADER_SIZE))
	       + MDA_HEADER_SIZE;
}

static int _raw_holds_vgname(struct format_instance *fid,
			     struct device_area *dev_area, const char *vgname)
{
	int r = 0;
	int noprecommit = 0;
	struct mda_header *mdah;

	if (!dev_open_readonly(dev_area->dev))
		return_0;

	if (!(mdah = raw_read_mda_header(fid->fmt, dev_area)))
		return_0;

	if (_find_vg_rlocn(dev_area, mdah, vgname, &noprecommit))
		r = 1;

	if (!dev_close(dev_area->dev))
		stack;

	return r;
}

static struct volume_group *_vg_read_raw_area(struct format_instance *fid,
					      const char *vgname,
					      struct device_area *area,
					      int precommitted)
{
	struct volume_group *vg = NULL;
	struct raw_locn *rlocn;
	struct mda_header *mdah;
	time_t when;
	char *desc;
	uint32_t wrap = 0;

	if (!(mdah = raw_read_mda_header(fid->fmt, area)))
		goto_out;

	if (!(rlocn = _find_vg_rlocn(area, mdah, vgname, &precommitted))) {
		log_debug("VG %s not found on %s", vgname, dev_name(area->dev));
		goto out;
	}

	if (rlocn->offset + rlocn->size > mdah->size)
		wrap = (uint32_t) ((rlocn->offset + rlocn->size) - mdah->size);

	if (wrap > rlocn->offset) {
		log_error("VG %s metadata too large for circular buffer",
			  vgname);
		goto out;
	}

	/* FIXME 64-bit */
	if (!(vg = text_vg_import_fd(fid, NULL, area->dev,
				     (off_t) (area->start + rlocn->offset),
				     (uint32_t) (rlocn->size - wrap),
				     (off_t) (area->start + MDA_HEADER_SIZE),
				     wrap, calc_crc, rlocn->checksum, &when,
				     &desc)))
		goto_out;
	log_debug("Read %s %smetadata (%u) from %s at %" PRIu64 " size %"
		  PRIu64, vg->name, precommitted ? "pre-commit " : "",
		  vg->seqno, dev_name(area->dev),
		  area->start + rlocn->offset, rlocn->size);

	if (precommitted)
		vg->status |= PRECOMMITTED;

      out:
	return vg;
}

static struct volume_group *_vg_read_raw(struct format_instance *fid,
					 const char *vgname,
					 struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct volume_group *vg;

	if (!dev_open_readonly(mdac->area.dev))
		return_NULL;

	vg = _vg_read_raw_area(fid, vgname, &mdac->area, 0);

	if (!dev_close(mdac->area.dev))
		stack;

	return vg;
}

static struct volume_group *_vg_read_precommit_raw(struct format_instance *fid,
						   const char *vgname,
						   struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct volume_group *vg;

	if (!dev_open_readonly(mdac->area.dev))
		return_NULL;

	vg = _vg_read_raw_area(fid, vgname, &mdac->area, 1);

	if (!dev_close(mdac->area.dev))
		stack;

	return vg;
}

static int _vg_write_raw(struct format_instance *fid, struct volume_group *vg,
			 struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct text_fid_context *fidtc = (struct text_fid_context *) fid->private;
	struct raw_locn *rlocn;
	struct mda_header *mdah;
	struct pv_list *pvl;
	int r = 0;
       uint64_t new_wrap = 0, old_wrap = 0, new_end;
	int found = 0;
	int noprecommit = 0;

	/* Ignore any mda on a PV outside the VG. vgsplit relies on this */
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (pvl->pv->dev == mdac->area.dev) {
			found = 1;
			break;
		}
	}

	if (!found)
		return 1;

	if (!dev_open(mdac->area.dev))
		return_0;

	if (!(mdah = raw_read_mda_header(fid->fmt, &mdac->area)))
		goto_out;

	rlocn = _find_vg_rlocn(&mdac->area, mdah,
			vg->old_name ? vg->old_name : vg->name, &noprecommit);
	mdac->rlocn.offset = _next_rlocn_offset(rlocn, mdah);

	if (!fidtc->raw_metadata_buf &&
	    !(fidtc->raw_metadata_buf_size =
			text_vg_export_raw(vg, "", &fidtc->raw_metadata_buf))) {
		log_error("VG %s metadata writing failed", vg->name);
		goto out;
	}

	mdac->rlocn.size = fidtc->raw_metadata_buf_size;

	if (mdac->rlocn.offset + mdac->rlocn.size > mdah->size)
		new_wrap = (mdac->rlocn.offset + mdac->rlocn.size) - mdah->size;

	if (rlocn && (rlocn->offset + rlocn->size > mdah->size))
		old_wrap = (rlocn->offset + rlocn->size) - mdah->size;

	new_end = new_wrap ? new_wrap + MDA_HEADER_SIZE :
			    mdac->rlocn.offset + mdac->rlocn.size;

	if ((new_wrap && old_wrap) ||
	    (rlocn && (new_wrap || old_wrap) && (new_end > rlocn->offset)) ||
	    (mdac->rlocn.size >= mdah->size)) {
		log_error("VG %s metadata too large for circular buffer",
			  vg->name);
		goto out;
	}

	log_debug("Writing %s metadata to %s at %" PRIu64 " len %" PRIu64,
		  vg->name, dev_name(mdac->area.dev), mdac->area.start +
		  mdac->rlocn.offset, mdac->rlocn.size - new_wrap);

	/* Write text out, circularly */
	if (!dev_write(mdac->area.dev, mdac->area.start + mdac->rlocn.offset,
		       (size_t) (mdac->rlocn.size - new_wrap),
		       fidtc->raw_metadata_buf))
		goto_out;

	if (new_wrap) {
               log_debug("Writing metadata to %s at %" PRIu64 " len %" PRIu64,
			  dev_name(mdac->area.dev), mdac->area.start +
			  MDA_HEADER_SIZE, new_wrap);

		if (!dev_write(mdac->area.dev,
			       mdac->area.start + MDA_HEADER_SIZE,
			       (size_t) new_wrap,
			       fidtc->raw_metadata_buf +
			       mdac->rlocn.size - new_wrap))
			goto_out;
	}

	mdac->rlocn.checksum = calc_crc(INITIAL_CRC, (uint8_t *)fidtc->raw_metadata_buf,
					(uint32_t) (mdac->rlocn.size -
						    new_wrap));
	if (new_wrap)
		mdac->rlocn.checksum = calc_crc(mdac->rlocn.checksum,
						(uint8_t *)fidtc->raw_metadata_buf +
						mdac->rlocn.size -
						new_wrap, (uint32_t) new_wrap);

	r = 1;

      out:
	if (!r) {
		if (!dev_close(mdac->area.dev))
			stack;

		if (fidtc->raw_metadata_buf) {
			dm_free(fidtc->raw_metadata_buf);
			fidtc->raw_metadata_buf = NULL;
		}
	}

	return r;
}

static int _vg_commit_raw_rlocn(struct format_instance *fid,
				struct volume_group *vg,
				struct metadata_area *mda,
				int precommit)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct text_fid_context *fidtc = (struct text_fid_context *) fid->private;
	struct mda_header *mdah;
	struct raw_locn *rlocn;
	struct pv_list *pvl;
	int r = 0;
	int found = 0;
	int noprecommit = 0;

	/* Ignore any mda on a PV outside the VG. vgsplit relies on this */
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (pvl->pv->dev == mdac->area.dev) {
			found = 1;
			break;
		}
	}

	if (!found)
		return 1;

	if (!(mdah = raw_read_mda_header(fid->fmt, &mdac->area)))
		goto_out;

	if (!(rlocn = _find_vg_rlocn(&mdac->area, mdah,
				     vg->old_name ? vg->old_name : vg->name,
				     &noprecommit))) {
		mdah->raw_locns[0].offset = 0;
		mdah->raw_locns[0].size = 0;
		mdah->raw_locns[0].checksum = 0;
		mdah->raw_locns[1].offset = 0;
		mdah->raw_locns[1].size = 0;
		mdah->raw_locns[1].checksum = 0;
		mdah->raw_locns[2].offset = 0;
		mdah->raw_locns[2].size = 0;
		mdah->raw_locns[2].checksum = 0;
		rlocn = &mdah->raw_locns[0];
	}

	if (precommit)
		rlocn++;
	else {
		/* If not precommitting, wipe the precommitted rlocn */
		mdah->raw_locns[1].offset = 0;
		mdah->raw_locns[1].size = 0;
		mdah->raw_locns[1].checksum = 0;
	}

	/* Is there new metadata to commit? */
	if (mdac->rlocn.size) {
		rlocn->offset = mdac->rlocn.offset;
		rlocn->size = mdac->rlocn.size;
		rlocn->checksum = mdac->rlocn.checksum;
		log_debug("%sCommitting %s metadata (%u) to %s header at %"
			  PRIu64, precommit ? "Pre-" : "", vg->name, vg->seqno,
			  dev_name(mdac->area.dev), mdac->area.start);
	} else
		log_debug("Wiping pre-committed %s metadata from %s "
			  "header at %" PRIu64, vg->name,
			  dev_name(mdac->area.dev), mdac->area.start);

	rlocn_set_ignored(mdah->raw_locns, mda_is_ignored(mda));
	if (!_raw_write_mda_header(fid->fmt, mdac->area.dev, mdac->area.start,
				   mdah)) {
		dm_pool_free(fid->fmt->cmd->mem, mdah);
		log_error("Failed to write metadata area header");
		goto out;
	}

	r = 1;

      out:
	if (!precommit) {
		if (!dev_close(mdac->area.dev))
			stack;
		if (fidtc->raw_metadata_buf) {
			dm_free(fidtc->raw_metadata_buf);
			fidtc->raw_metadata_buf = NULL;
		}
	}

	return r;
}

static int _vg_commit_raw(struct format_instance *fid, struct volume_group *vg,
			  struct metadata_area *mda)
{
	return _vg_commit_raw_rlocn(fid, vg, mda, 0);
}

static int _vg_precommit_raw(struct format_instance *fid,
			     struct volume_group *vg,
			     struct metadata_area *mda)
{
	return _vg_commit_raw_rlocn(fid, vg, mda, 1);
}

/* Close metadata area devices */
static int _vg_revert_raw(struct format_instance *fid, struct volume_group *vg,
			  struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct pv_list *pvl;
	int found = 0;

	/* Ignore any mda on a PV outside the VG. vgsplit relies on this */
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (pvl->pv->dev == mdac->area.dev) {
			found = 1;
			break;
		}
	}

	if (!found)
		return 1;

	/* Wipe pre-committed metadata */
	mdac->rlocn.size = 0;
	return _vg_commit_raw_rlocn(fid, vg, mda, 0);
}

static int _vg_remove_raw(struct format_instance *fid, struct volume_group *vg,
			  struct metadata_area *mda)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct mda_header *mdah;
	struct raw_locn *rlocn;
	int r = 0;
	int noprecommit = 0;

	if (!dev_open(mdac->area.dev))
		return_0;

	if (!(mdah = raw_read_mda_header(fid->fmt, &mdac->area)))
		goto_out;

	if (!(rlocn = _find_vg_rlocn(&mdac->area, mdah, vg->name, &noprecommit))) {
		rlocn = &mdah->raw_locns[0];
		mdah->raw_locns[1].offset = 0;
	}

	rlocn->offset = 0;
	rlocn->size = 0;
	rlocn->checksum = 0;
	rlocn_set_ignored(mdah->raw_locns, mda_is_ignored(mda));

	if (!_raw_write_mda_header(fid->fmt, mdac->area.dev, mdac->area.start,
				   mdah)) {
		dm_pool_free(fid->fmt->cmd->mem, mdah);
		log_error("Failed to write metadata area header");
		goto out;
	}

	r = 1;

      out:
	if (!dev_close(mdac->area.dev))
		stack;

	return r;
}

static struct volume_group *_vg_read_file_name(struct format_instance *fid,
					       const char *vgname,
					       const char *read_path)
{
	struct volume_group *vg;
	time_t when;
	char *desc;

	if (!(vg = text_vg_import_file(fid, read_path, &when, &desc)))
		return_NULL;

	/*
	 * Currently you can only have a single volume group per
	 * text file (this restriction may remain).  We need to
	 * check that it contains the correct volume group.
	 */
	if (vgname && strcmp(vgname, vg->name)) {
		release_vg(vg);
		log_error("'%s' does not contain volume group '%s'.",
			  read_path, vgname);
		return NULL;
	} else
		log_debug("Read volume group %s from %s", vg->name, read_path);

	return vg;
}

static struct volume_group *_vg_read_file(struct format_instance *fid,
					  const char *vgname,
					  struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;

	return _vg_read_file_name(fid, vgname, tc->path_live);
}

static struct volume_group *_vg_read_precommit_file(struct format_instance *fid,
						    const char *vgname,
						    struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;
	struct volume_group *vg;

	if ((vg = _vg_read_file_name(fid, vgname, tc->path_edit)))
		vg->status |= PRECOMMITTED;
	else
		vg = _vg_read_file_name(fid, vgname, tc->path_live);

	return vg;
}

static int _vg_write_file(struct format_instance *fid __attribute__((unused)),
			  struct volume_group *vg, struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;

	FILE *fp;
	int fd;
	char *slash;
	char temp_file[PATH_MAX], temp_dir[PATH_MAX];

	slash = strrchr(tc->path_edit, '/');

	if (slash == 0)
		strcpy(temp_dir, ".");
	else if (slash - tc->path_edit < PATH_MAX) {
		strncpy(temp_dir, tc->path_edit,
			(size_t) (slash - tc->path_edit));
		temp_dir[slash - tc->path_edit] = '\0';

	} else {
		log_error("Text format failed to determine directory.");
		return 0;
	}

	if (!create_temp_name(temp_dir, temp_file, sizeof(temp_file), &fd,
			      &vg->cmd->rand_seed)) {
		log_error("Couldn't create temporary text file name.");
		return 0;
	}

	if (!(fp = fdopen(fd, "w"))) {
		log_sys_error("fdopen", temp_file);
		if (close(fd))
			log_sys_error("fclose", temp_file);
		return 0;
	}

	log_debug("Writing %s metadata to %s", vg->name, temp_file);

	if (!text_vg_export_file(vg, tc->desc, fp)) {
		log_error("Failed to write metadata to %s.", temp_file);
		if (fclose(fp))
			log_sys_error("fclose", temp_file);
		return 0;
	}

	if (fsync(fd) && (errno != EROFS) && (errno != EINVAL)) {
		log_sys_error("fsync", tc->path_edit);
		if (fclose(fp))
			log_sys_error("fclose", tc->path_edit);
		return 0;
	}

	if (lvm_fclose(fp, tc->path_edit))
		return_0;

	if (rename(temp_file, tc->path_edit)) {
		log_debug("Renaming %s to %s", temp_file, tc->path_edit);
		log_error("%s: rename to %s failed: %s", temp_file,
			  tc->path_edit, strerror(errno));
		return 0;
	}

	return 1;
}

static int _vg_commit_file_backup(struct format_instance *fid __attribute__((unused)),
				  struct volume_group *vg,
				  struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;

	if (test_mode()) {
		log_verbose("Test mode: Skipping committing %s metadata (%u)",
			    vg->name, vg->seqno);
		if (unlink(tc->path_edit)) {
			log_debug("Unlinking %s", tc->path_edit);
			log_sys_error("unlink", tc->path_edit);
			return 0;
		}
	} else {
		log_debug("Committing %s metadata (%u)", vg->name, vg->seqno);
		log_debug("Renaming %s to %s", tc->path_edit, tc->path_live);
		if (rename(tc->path_edit, tc->path_live)) {
			log_error("%s: rename to %s failed: %s", tc->path_edit,
				  tc->path_live, strerror(errno));
			return 0;
		}
	}

	sync_dir(tc->path_edit);

	return 1;
}

static int _vg_commit_file(struct format_instance *fid, struct volume_group *vg,
			   struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;
	const char *slash;
	char new_name[PATH_MAX];
	size_t len;

	if (!_vg_commit_file_backup(fid, vg, mda))
		return 0;

	/* vgrename? */
	if ((slash = strrchr(tc->path_live, '/')))
		slash = slash + 1;
	else
		slash = tc->path_live;

	if (strcmp(slash, vg->name)) {
		len = slash - tc->path_live;
		strncpy(new_name, tc->path_live, len);
		strcpy(new_name + len, vg->name);
		log_debug("Renaming %s to %s", tc->path_live, new_name);
		if (test_mode())
			log_verbose("Test mode: Skipping rename");
		else {
			if (rename(tc->path_live, new_name)) {
				log_error("%s: rename to %s failed: %s",
					  tc->path_live, new_name,
					  strerror(errno));
				sync_dir(new_name);
				return 0;
			}
		}
	}

	return 1;
}

static int _vg_remove_file(struct format_instance *fid __attribute__((unused)),
			   struct volume_group *vg __attribute__((unused)),
			   struct metadata_area *mda)
{
	struct text_context *tc = (struct text_context *) mda->metadata_locn;

	if (path_exists(tc->path_edit) && unlink(tc->path_edit)) {
		log_sys_error("unlink", tc->path_edit);
		return 0;
	}

	if (path_exists(tc->path_live) && unlink(tc->path_live)) {
		log_sys_error("unlink", tc->path_live);
		return 0;
	}

	sync_dir(tc->path_live);

	return 1;
}

static int _scan_file(const struct format_type *fmt, const char *vgname)
{
	struct dirent *dirent;
	struct dir_list *dl;
	struct dm_list *dir_list;
	char *tmp;
	DIR *d;
	struct volume_group *vg;
	struct format_instance *fid;
	struct format_instance_ctx fic;
	char path[PATH_MAX];
	char *scanned_vgname;

	dir_list = &((struct mda_lists *) fmt->private)->dirs;

	dm_list_iterate_items(dl, dir_list) {
		if (!(d = opendir(dl->dir))) {
			log_sys_error("opendir", dl->dir);
			continue;
		}
		while ((dirent = readdir(d)))
			if (strcmp(dirent->d_name, ".") &&
			    strcmp(dirent->d_name, "..") &&
			    (!(tmp = strstr(dirent->d_name, ".tmp")) ||
			     tmp != dirent->d_name + strlen(dirent->d_name)
			     - 4)) {
				scanned_vgname = dirent->d_name;

				/* If vgname supplied, only scan that one VG */
				if (vgname && strcmp(vgname, scanned_vgname))
					continue;

				if (dm_snprintf(path, PATH_MAX, "%s/%s",
						 dl->dir, scanned_vgname) < 0) {
					log_error("Name too long %s/%s",
						  dl->dir, scanned_vgname);
					break;
				}

				/* FIXME stat file to see if it's changed */
				/* FIXME: Check this fid is OK! */
				fic.type = FMT_INSTANCE_PRIVATE_MDAS;
				fic.context.private = NULL;
				fid = _text_create_text_instance(fmt, &fic);
				if ((vg = _vg_read_file_name(fid, scanned_vgname,
							     path))) {
					/* FIXME Store creation host in vg */
					lvmcache_update_vg(vg, 0);
					release_vg(vg);
				}
			}

		if (closedir(d))
			log_sys_error("closedir", dl->dir);
	}

	return 1;
}

const char *vgname_from_mda(const struct format_type *fmt,
			    struct mda_header *mdah,
			    struct device_area *dev_area, struct id *vgid,
			    uint64_t *vgstatus, char **creation_host,
			    uint64_t *mda_free_sectors)
{
	struct raw_locn *rlocn;
	uint32_t wrap = 0;
	const char *vgname = NULL;
	unsigned int len = 0;
	char buf[NAME_LEN + 1] __attribute__((aligned(8)));
	char uuid[64] __attribute__((aligned(8)));
	uint64_t buffer_size, current_usage;

	if (mda_free_sectors)
		*mda_free_sectors = ((dev_area->size - MDA_HEADER_SIZE) / 2) >> SECTOR_SHIFT;

	if (!mdah) {
		log_error(INTERNAL_ERROR "vgname_from_mda called with NULL pointer for mda_header");
		goto_out;
	}

	/* FIXME Cope with returning a list */
	rlocn = mdah->raw_locns;

	/*
	 * If no valid offset, do not try to search for vgname
	 */
	if (!rlocn->offset)
		goto out;

	/* Do quick check for a vgname */
	if (!dev_read(dev_area->dev, dev_area->start + rlocn->offset,
		      NAME_LEN, buf))
		goto_out;

	while (buf[len] && !isspace(buf[len]) && buf[len] != '{' &&
	       len < (NAME_LEN - 1))
		len++;

	buf[len] = '\0';

	/* Ignore this entry if the characters aren't permissible */
	if (!validate_name(buf))
		goto_out;

	/* We found a VG - now check the metadata */
	if (rlocn->offset + rlocn->size > mdah->size)
		wrap = (uint32_t) ((rlocn->offset + rlocn->size) - mdah->size);

	if (wrap > rlocn->offset) {
		log_error("%s: metadata too large for circular buffer",
			  dev_name(dev_area->dev));
		goto out;
	}

	/* FIXME 64-bit */
	if (!(vgname = text_vgname_import(fmt, dev_area->dev,
					  (off_t) (dev_area->start +
						   rlocn->offset),
					  (uint32_t) (rlocn->size - wrap),
					  (off_t) (dev_area->start +
						   MDA_HEADER_SIZE),
					  wrap, calc_crc, rlocn->checksum,
					  vgid, vgstatus, creation_host)))
		goto_out;

	/* Ignore this entry if the characters aren't permissible */
	if (!validate_name(vgname)) {
		vgname = NULL;
		goto_out;
	}

	if (!id_write_format(vgid, uuid, sizeof(uuid))) {
		vgname = NULL;
		goto_out;
	}

	log_debug("%s: Found metadata at %" PRIu64 " size %" PRIu64
		  " (in area at %" PRIu64 " size %" PRIu64
		  ") for %s (%s)",
		  dev_name(dev_area->dev), dev_area->start + rlocn->offset,
		  rlocn->size, dev_area->start, dev_area->size, vgname, uuid);

	if (mda_free_sectors) {
		current_usage = (rlocn->size + SECTOR_SIZE - UINT64_C(1)) -
				 (rlocn->size + SECTOR_SIZE - UINT64_C(1)) % SECTOR_SIZE;
		buffer_size = mdah->size - MDA_HEADER_SIZE;

		if (current_usage * 2 >= buffer_size)
			*mda_free_sectors = UINT64_C(0);
		else
			*mda_free_sectors = ((buffer_size - 2 * current_usage) / 2) >> SECTOR_SHIFT;
	}

      out:
	return vgname;
}

static int _scan_raw(const struct format_type *fmt, const char *vgname __attribute__((unused)))
{
	struct raw_list *rl;
	struct dm_list *raw_list;
	const char *scanned_vgname;
	struct volume_group *vg;
	struct format_instance fid;
	struct id vgid;
	uint64_t vgstatus;
	struct mda_header *mdah;

	raw_list = &((struct mda_lists *) fmt->private)->raws;

	fid.fmt = fmt;
	dm_list_init(&fid.metadata_areas_in_use);
	dm_list_init(&fid.metadata_areas_ignored);

	dm_list_iterate_items(rl, raw_list) {
		/* FIXME We're reading mdah twice here... */
		if (!dev_open_readonly(rl->dev_area.dev)) {
			stack;
			continue;
		}

		if (!(mdah = raw_read_mda_header(fmt, &rl->dev_area))) {
			stack;
			goto close_dev;
		}

		if ((scanned_vgname = vgname_from_mda(fmt, mdah,
					      &rl->dev_area, &vgid, &vgstatus,
					      NULL, NULL))) {
			vg = _vg_read_raw_area(&fid, scanned_vgname, &rl->dev_area, 0);
			if (vg)
				lvmcache_update_vg(vg, 0);

		}
	close_dev:
		if (!dev_close(rl->dev_area.dev))
			stack;
	}

	return 1;
}

static int _text_scan(const struct format_type *fmt, const char *vgname)
{
	return (_scan_file(fmt, vgname) & _scan_raw(fmt, vgname));
}

struct _write_single_mda_baton {
	const struct format_type *fmt;
	struct physical_volume *pv;
};

static int _write_single_mda(struct metadata_area *mda, void *baton)
{
	struct _write_single_mda_baton *p = baton;
	struct mda_context *mdac;

	char buf[MDA_HEADER_SIZE] __attribute__((aligned(8)));
	struct mda_header *mdah = (struct mda_header *) buf;

	mdac = mda->metadata_locn;
	memset(&buf, 0, sizeof(buf));
	mdah->size = mdac->area.size;
	rlocn_set_ignored(mdah->raw_locns, mda_is_ignored(mda));

	if (!_raw_write_mda_header(p->fmt, mdac->area.dev,
				   mdac->area.start, mdah)) {
		if (!dev_close(p->pv->dev))
			stack;
		return_0;
	}
	return 1;
}

/* Only for orphans */
static int _text_pv_write(const struct format_type *fmt, struct physical_volume *pv)
{
	struct format_instance *fid = pv->fid;
	const char *pvid = (const char *) (*pv->old_id.uuid ? &pv->old_id : &pv->id);
	struct label *label;
	struct lvmcache_info *info;
	struct mda_context *mdac;
	struct metadata_area *mda;
	struct _write_single_mda_baton baton;
	unsigned mda_index;

	/* Add a new cache entry with PV info or update existing one. */
	if (!(info = lvmcache_add(fmt->labeller, (const char *) &pv->id,
				  pv->dev, pv->vg_name, NULL, 0)))
		return_0;

	label = lvmcache_get_label(info);
	label->sector = pv->label_sector;

	lvmcache_update_pv(info, pv, fmt);

	/* Flush all cached metadata areas, we will reenter new/modified ones. */
	lvmcache_del_mdas(info);

	/*
	 * Add all new or modified metadata areas for this PV stored in
	 * its format instance. If this PV is not part of a VG yet,
	 * pv->fid will be used. Otherwise pv->vg->fid will be used.
	 * The fid_get_mda_indexed fn can handle that transparently,
	 * just pass the right format_instance in.
	 */
	for (mda_index = 0; mda_index < FMT_TEXT_MAX_MDAS_PER_PV; mda_index++) {
		if (!(mda = fid_get_mda_indexed(fid, pvid, ID_LEN, mda_index)))
			continue;

		mdac = (struct mda_context *) mda->metadata_locn;
		log_debug("Creating metadata area on %s at sector %"
			  PRIu64 " size %" PRIu64 " sectors",
			  dev_name(mdac->area.dev),
			  mdac->area.start >> SECTOR_SHIFT,
			  mdac->area.size >> SECTOR_SHIFT);

		// if fmt is not the same as info->fmt we are in trouble
		lvmcache_add_mda(info, mdac->area.dev,
				 mdac->area.start, mdac->area.size, mda_is_ignored(mda));
	}

	/*
	 * FIXME: Allow writing zero offset/size data area to disk.
	 *        This requires defining a special value since we can't
	 *        write offset/size that is 0/0 - this is already reserved
	 *        as a delimiter in data/metadata area area list in PV header
	 *        (needs exploring compatibility with older lvm2).
	 */

	/*
	 * We can't actually write pe_start = 0 (a data area offset)
	 * in PV header now. We need to replace this value here. This can
	 * happen with vgcfgrestore with redefined pe_start or
	 * pvcreate --restorefile. However, we can can have this value in
	 * metadata which will override the value in the PV header.
	 */

	if (!lvmcache_update_das(info, pv))
		return_0;

	if (!dev_open(pv->dev))
		return_0;

	baton.pv = pv;
	baton.fmt = fmt;

	if (!lvmcache_foreach_mda(info, _write_single_mda, &baton))
		return_0;

	if (!label_write(pv->dev, label)) {
		dev_close(pv->dev);
		return_0;
	}

	/*
	 *  FIXME: We should probably use the format instance's metadata
	 *        areas for label_write and only if it's successful,
	 *        update the cache afterwards?
	 */

	if (!dev_close(pv->dev))
		return_0;

	return 1;
}

static int _add_raw(struct dm_list *raw_list, struct device_area *dev_area)
{
	struct raw_list *rl;

	/* Already present? */
	dm_list_iterate_items(rl, raw_list) {
		/* FIXME Check size/overlap consistency too */
		if (rl->dev_area.dev == dev_area->dev &&
		    rl->dev_area.start == dev_area->start)
			return 1;
	}

	if (!(rl = dm_malloc(sizeof(struct raw_list)))) {
		log_error("_add_raw allocation failed");
		return 0;
	}
	memcpy(&rl->dev_area, dev_area, sizeof(*dev_area));
	dm_list_add(raw_list, &rl->list);

	return 1;
}

/*
 * Copy constructor for a metadata_locn.
 */
static void *_metadata_locn_copy_raw(struct dm_pool *mem, void *metadata_locn)
{
	struct mda_context *mdac, *mdac_new;

	mdac = (struct mda_context *) metadata_locn;
	if (!(mdac_new = dm_pool_alloc(mem, sizeof(*mdac_new)))) {
		log_error("mda_context allocation failed");
		return NULL;
	}
	memcpy(mdac_new, mdac, sizeof(*mdac));

	return mdac_new;
}

/*
 * Return a string description of the metadata location.
 */
static const char *_metadata_locn_name_raw(void *metadata_locn)
{
	struct mda_context *mdac = (struct mda_context *) metadata_locn;

	return dev_name(mdac->area.dev);
}

static uint64_t _metadata_locn_offset_raw(void *metadata_locn)
{
	struct mda_context *mdac = (struct mda_context *) metadata_locn;

	return mdac->area.start;
}

static int _text_pv_read(const struct format_type *fmt, const char *pv_name,
		    struct physical_volume *pv, int scan_label_only)
{
	struct label *label;
	struct device *dev;

	if (!(dev = dev_cache_get(pv_name, fmt->cmd->filter)))
		return_0;

	if (!(label_read(dev, &label, UINT64_C(0))))
		return_0;

	if (!lvmcache_populate_pv_fields(label->info, pv, scan_label_only))
		return 0;

	return 1;
}

static int _text_pv_initialise(const struct format_type *fmt,
			       const int64_t label_sector,
			       uint64_t pe_start,
			       uint32_t extent_count,
			       uint32_t extent_size,
			       unsigned long data_alignment,
			       unsigned long data_alignment_offset,
			       struct physical_volume *pv)
{
	/*
	 * Try to keep the value of PE start set to a firm value if requested.
	 * This is usefull when restoring existing PE start value (backups etc.).
	 */
	if (pe_start != PV_PE_START_CALC)
		pv->pe_start = pe_start;

	if (!data_alignment)
		data_alignment = find_config_tree_int(pv->fmt->cmd,
					      "devices/data_alignment",
					      0) * 2;

	if (set_pe_align(pv, data_alignment) != data_alignment &&
	    data_alignment) {
		log_error("%s: invalid data alignment of "
			  "%lu sectors (requested %lu sectors)",
			  pv_dev_name(pv), pv->pe_align, data_alignment);
		return 0;
	}

	if (set_pe_align_offset(pv, data_alignment_offset) != data_alignment_offset &&
	    data_alignment_offset) {
		log_error("%s: invalid data alignment offset of "
			  "%lu sectors (requested %lu sectors)",
			  pv_dev_name(pv), pv->pe_align_offset, data_alignment_offset);
		return 0;
	}

	if (pv->pe_align < pv->pe_align_offset) {
		log_error("%s: pe_align (%lu sectors) must not be less "
			  "than pe_align_offset (%lu sectors)",
			  pv_dev_name(pv), pv->pe_align, pv->pe_align_offset);
		return 0;
	}

	if (pe_start == PV_PE_START_CALC && pv->pe_start < pv->pe_align)
		pv->pe_start = pv->pe_align;

	if (extent_size)
		pv->pe_size = extent_size;

	if (extent_count)
		pv->pe_count = extent_count;

	if ((pv->pe_start + pv->pe_count * pv->pe_size - 1) > (pv->size << SECTOR_SHIFT)) {
		log_error("Physical extents end beyond end of device %s.",
			   pv_dev_name(pv));
		return 0;
	}

	if (label_sector != -1)
                pv->label_sector = label_sector;

	return 1;
}

static void _text_destroy_instance(struct format_instance *fid)
{
	if (--fid->ref_count <= 1) {
		if (fid->metadata_areas_index)
			dm_hash_destroy(fid->metadata_areas_index);
		dm_pool_destroy(fid->mem);
	}
}

static void _free_dirs(struct dm_list *dir_list)
{
	struct dm_list *dl, *tmp;

	dm_list_iterate_safe(dl, tmp, dir_list) {
		dm_list_del(dl);
		dm_free(dl);
	}
}

static void _free_raws(struct dm_list *raw_list)
{
	struct dm_list *rl, *tmp;

	dm_list_iterate_safe(rl, tmp, raw_list) {
		dm_list_del(rl);
		dm_free(rl);
	}
}

static void _text_destroy(struct format_type *fmt)
{
	/* FIXME out of place, but the main (cmd) pool has been already
	 * destroyed and touching the fid (also via release_vg) will crash the
	 * program */
	dm_hash_destroy(fmt->orphan_vg->fid->metadata_areas_index);
	dm_hash_destroy(fmt->orphan_vg->hostnames);
	dm_pool_destroy(fmt->orphan_vg->fid->mem);
	dm_pool_destroy(fmt->orphan_vg->vgmem);

	if (fmt->private) {
		_free_dirs(&((struct mda_lists *) fmt->private)->dirs);
		_free_raws(&((struct mda_lists *) fmt->private)->raws);
		dm_free(fmt->private);
	}

	dm_free(fmt);
}

static struct metadata_area_ops _metadata_text_file_ops = {
	.vg_read = _vg_read_file,
	.vg_read_precommit = _vg_read_precommit_file,
	.vg_write = _vg_write_file,
	.vg_remove = _vg_remove_file,
	.vg_commit = _vg_commit_file
};

static struct metadata_area_ops _metadata_text_file_backup_ops = {
	.vg_read = _vg_read_file,
	.vg_write = _vg_write_file,
	.vg_remove = _vg_remove_file,
	.vg_commit = _vg_commit_file_backup
};

static struct metadata_area_ops _metadata_text_raw_ops = {
	.vg_read = _vg_read_raw,
	.vg_read_precommit = _vg_read_precommit_raw,
	.vg_write = _vg_write_raw,
	.vg_remove = _vg_remove_raw,
	.vg_precommit = _vg_precommit_raw,
	.vg_commit = _vg_commit_raw,
	.vg_revert = _vg_revert_raw,
	.mda_metadata_locn_copy = _metadata_locn_copy_raw,
	.mda_metadata_locn_name = _metadata_locn_name_raw,
	.mda_metadata_locn_offset = _metadata_locn_offset_raw,
	.mda_free_sectors = _mda_free_sectors_raw,
	.mda_total_sectors = _mda_total_sectors_raw,
	.mda_in_vg = _mda_in_vg_raw,
	.pv_analyze_mda = _pv_analyze_mda_raw,
	.mda_locns_match = _mda_locns_match_raw,
	.mda_get_device = _mda_get_device_raw
};

static int _text_pv_setup(const struct format_type *fmt,
			  struct physical_volume *pv,
			  struct volume_group *vg)
{
	struct format_instance *fid = pv->fid;
	const char *pvid = (const char *) (*pv->old_id.uuid ? &pv->old_id : &pv->id);
	struct lvmcache_info *info;
	unsigned mda_index;
	struct metadata_area *pv_mda, *pv_mda_copy;
	struct mda_context *pv_mdac;
	uint64_t pe_count;
	uint64_t size_reduction = 0;

	/* If PV has its own format instance, add mdas from pv->fid to vg->fid. */
	if (pv->fid != vg->fid) {
		for (mda_index = 0; mda_index < FMT_TEXT_MAX_MDAS_PER_PV; mda_index++) {
			if (!(pv_mda = fid_get_mda_indexed(fid, pvid, ID_LEN, mda_index)))
				continue;

			/* Be sure it's not already in VG's format instance! */
			if (!fid_get_mda_indexed(vg->fid, pvid, ID_LEN, mda_index)) {
				pv_mda_copy = mda_copy(vg->fid->mem, pv_mda);
				fid_add_mda(vg->fid, pv_mda_copy, pvid, ID_LEN, mda_index);
			}
		}
	}
	/*
	 * Otherwise, if the PV is already a part of the VG (pv->fid == vg->fid),
	 * reread PV mda information from the cache and add it to vg->fid.
	 */
	else {
		if (!(info = lvmcache_info_from_pvid(pv->dev->pvid, 0))) {
			log_error("PV %s missing from cache", pv_dev_name(pv));
			return 0;
		}

		if (!lvmcache_check_format(info, fmt))
			return_0;

		if (!lvmcache_fid_add_mdas_pv(info, fid))
			return_0;
	}

	/* If there's the 2nd mda, we need to reduce
	 * usable size for further pe_count calculation! */
	if ((pv_mda = fid_get_mda_indexed(fid, pvid, ID_LEN, 1)) &&
	    (pv_mdac = pv_mda->metadata_locn))
		size_reduction = pv_mdac->area.size >> SECTOR_SHIFT;

	/* From now on, VG format instance will be used. */
	pv_set_fid(pv, vg->fid);

	/* FIXME Cope with genuine pe_count 0 */

	/* If missing, estimate pv->size from file-based metadata */
	if (!pv->size && pv->pe_count)
		pv->size = pv->pe_count * (uint64_t) vg->extent_size +
			   pv->pe_start + size_reduction;

	/* Recalculate number of extents that will fit */
	if (!pv->pe_count) {
		pe_count = (pv->size - pv->pe_start - size_reduction) /
			   vg->extent_size;
		if (pe_count > UINT32_MAX) {
			log_error("PV %s too large for extent size %s.",
				  pv_dev_name(pv),
				  display_size(vg->cmd, (uint64_t) vg->extent_size));
			return 0;
		}
		pv->pe_count = (uint32_t) pe_count;
	}

	/* Unlike LVM1, we don't store this outside a VG */
	/* FIXME Default from config file? vgextend cmdline flag? */
	pv->status |= ALLOCATABLE_PV;

	return 1;
}

static void *_create_text_context(struct dm_pool *mem, struct text_context *tc)
{
	struct text_context *new_tc;
	const char *path;
	char *tmp;

	if (!tc)
		return NULL;

	path = tc->path_live;

	if ((tmp = strstr(path, ".tmp")) && (tmp == path + strlen(path) - 4)) {
		log_error("%s: Volume group filename may not end in .tmp",
			  path);
		return NULL;
	}

	if (!(new_tc = dm_pool_alloc(mem, sizeof(*new_tc))))
		return_NULL;

	if (!(new_tc->path_live = dm_pool_strdup(mem, path)))
		goto_bad;

	/* If path_edit not defined, create one from path_live with .tmp suffix. */
	if (!tc->path_edit) {
		if (!(tmp = dm_pool_alloc(mem, strlen(path) + 5)))
			goto_bad;
		sprintf(tmp, "%s.tmp", path);
		new_tc->path_edit = tmp;
	}
	else if (!(new_tc->path_edit = dm_pool_strdup(mem, tc->path_edit)))
		goto_bad;

	if (!(new_tc->desc = tc->desc ? dm_pool_strdup(mem, tc->desc)
				      : dm_pool_strdup(mem, "")))
		goto_bad;

	return (void *) new_tc;

      bad:
	dm_pool_free(mem, new_tc);

	log_error("Couldn't allocate text format context object.");
	return NULL;
}

static int _create_vg_text_instance(struct format_instance *fid,
                                    const struct format_instance_ctx *fic)
{
	static char path[PATH_MAX];
	uint32_t type = fic->type;
	struct text_fid_context *fidtc;
	struct metadata_area *mda;
	struct mda_context *mdac;
	struct dir_list *dl;
	struct raw_list *rl;
	struct dm_list *dir_list, *raw_list;
	struct text_context tc;
	struct lvmcache_vginfo *vginfo;
	const char *vg_name, *vg_id;

	if (!(fidtc = (struct text_fid_context *)
			dm_pool_zalloc(fid->mem, sizeof(*fidtc)))) {
		log_error("Couldn't allocate text_fid_context.");
		return 0;
	}

	fidtc->raw_metadata_buf = NULL;
	fid->private = (void *) fidtc;

	if (type & FMT_INSTANCE_PRIVATE_MDAS) {
		if (!(mda = dm_pool_zalloc(fid->mem, sizeof(*mda))))
			return_0;
		mda->ops = &_metadata_text_file_backup_ops;
		mda->metadata_locn = _create_text_context(fid->mem, fic->context.private);
		mda->status = 0;
		fid->metadata_areas_index = NULL;
		fid_add_mda(fid, mda, NULL, 0, 0);
	} else {
		vg_name = fic->context.vg_ref.vg_name;
		vg_id = fic->context.vg_ref.vg_id;

		if (!(fid->metadata_areas_index = dm_hash_create(128))) {
			log_error("Couldn't create metadata index for format "
				  "instance of VG %s.", vg_name);
			return 0;
		}

		if (type & FMT_INSTANCE_AUX_MDAS) {
			dir_list = &((struct mda_lists *) fid->fmt->private)->dirs;
			dm_list_iterate_items(dl, dir_list) {
				if (dm_snprintf(path, PATH_MAX, "%s/%s", dl->dir, vg_name) < 0) {
					log_error("Name too long %s/%s", dl->dir, vg_name);
					return 0;
				}

				if (!(mda = dm_pool_zalloc(fid->mem, sizeof(*mda))))
					return_0;
				mda->ops = &_metadata_text_file_ops;
				tc.path_live = path;
				tc.path_edit = tc.desc = NULL;
				mda->metadata_locn = _create_text_context(fid->mem, &tc);
				mda->status = 0;
				fid_add_mda(fid, mda, NULL, 0, 0);
			}

			raw_list = &((struct mda_lists *) fid->fmt->private)->raws;
			dm_list_iterate_items(rl, raw_list) {
				/* FIXME Cache this; rescan below if some missing */
				if (!_raw_holds_vgname(fid, &rl->dev_area, vg_name))
					continue;

				if (!(mda = dm_pool_zalloc(fid->mem, sizeof(*mda))))
					return_0;

				if (!(mdac = dm_pool_zalloc(fid->mem, sizeof(*mdac))))
					return_0;
				mda->metadata_locn = mdac;
				/* FIXME Allow multiple dev_areas inside area */
				memcpy(&mdac->area, &rl->dev_area, sizeof(mdac->area));
				mda->ops = &_metadata_text_raw_ops;
				mda->status = 0;
				/* FIXME MISTAKE? mda->metadata_locn = context; */
				fid_add_mda(fid, mda, NULL, 0, 0);
			}
		}

		if (type & FMT_INSTANCE_MDAS) {
			/* Scan PVs in VG for any further MDAs */
			lvmcache_label_scan(fid->fmt->cmd, 0);
			if (!(vginfo = lvmcache_vginfo_from_vgname(vg_name, vg_id)))
				goto_out;
			if (!lvmcache_fid_add_mdas_vg(vginfo, fid))
				goto_out;
		}

		/* FIXME Check raw metadata area count - rescan if required */
	}

out:
	return 1;
}

static int _add_metadata_area_to_pv(struct physical_volume *pv,
				    unsigned mda_index,
				    uint64_t mda_start,
				    uint64_t mda_size,
				    unsigned mda_ignored)
{
	struct metadata_area *mda;
	struct mda_context *mdac;
	struct mda_lists *mda_lists = (struct mda_lists *) pv->fmt->private;

	if (mda_index >= FMT_TEXT_MAX_MDAS_PER_PV) {
		log_error(INTERNAL_ERROR "can't add metadata area with "
					 "index %u to PV %s. Metadata "
					 "layout not supported by %s format.",
					  mda_index, dev_name(pv->dev),
					  pv->fmt->name);
	}

	if (!(mda = dm_pool_zalloc(pv->fid->mem, sizeof(struct metadata_area)))) {
		log_error("struct metadata_area allocation failed");
		return 0;
	}

	if (!(mdac = dm_pool_zalloc(pv->fid->mem, sizeof(struct mda_context)))) {
		log_error("struct mda_context allocation failed");
		dm_free(mda);
		return 0;
	}

	mda->ops = mda_lists->raw_ops;
	mda->metadata_locn = mdac;
	mda->status = 0;

	mdac->area.dev = pv->dev;
	mdac->area.start = mda_start;
	mdac->area.size = mda_size;
	mdac->free_sectors = UINT64_C(0);
	memset(&mdac->rlocn, 0, sizeof(mdac->rlocn));
	mda_set_ignored(mda, mda_ignored);

	fid_add_mda(pv->fid, mda, (char *) &pv->id, ID_LEN, mda_index);

	return 1;
}

static int _text_pv_remove_metadata_area(const struct format_type *fmt,
					 struct physical_volume *pv,
					 unsigned mda_index);

static int _text_pv_add_metadata_area(const struct format_type *fmt,
				      struct physical_volume *pv,
				      int pe_start_locked,
				      unsigned mda_index,
				      uint64_t mda_size,
				      unsigned mda_ignored)
{
	struct format_instance *fid = pv->fid;
	const char *pvid = (const char *) (*pv->old_id.uuid ? &pv->old_id : &pv->id);
	uint64_t pe_start, pe_end;
	uint64_t alignment, alignment_offset;
	uint64_t disk_size;
	uint64_t mda_start;
	uint64_t adjustment, limit, tmp_mda_size;
	uint64_t wipe_size = 8 << SECTOR_SHIFT;
	size_t page_size = lvm_getpagesize();
	struct metadata_area *mda;
	struct mda_context *mdac;
	const char *limit_name;
	int limit_applied = 0;

	if (mda_index >= FMT_TEXT_MAX_MDAS_PER_PV) {
		log_error(INTERNAL_ERROR "invalid index of value %u used "
			      "while trying to add metadata area on PV %s. "
			      "Metadata layout not supported by %s format.",
			       mda_index, pv_dev_name(pv), fmt->name);
		return 0;
	}

	pe_start = pv->pe_start << SECTOR_SHIFT;
	alignment = pv->pe_align << SECTOR_SHIFT;
	alignment_offset = pv->pe_align_offset << SECTOR_SHIFT;
	disk_size = pv->size << SECTOR_SHIFT;
	mda_size = mda_size << SECTOR_SHIFT;

	if (fid_get_mda_indexed(fid, pvid, ID_LEN, mda_index)) {
		if (!_text_pv_remove_metadata_area(fmt, pv, mda_index)) {
			log_error(INTERNAL_ERROR "metadata area with index %u already "
				  "exists on PV %s and removal failed.",
				  mda_index, pv_dev_name(pv));
			return 0;
		}
	}

	/* First metadata area at the start of the device. */
	if (mda_index == 0) {
		/*
		 * Try to fit MDA0 end within given pe_start limit if its value
		 * is locked. If it's not locked, count with any existing MDA1.
		 * If there's no MDA1, just use disk size as the limit.
		 */
		if (pe_start_locked) {
			limit = pe_start;
			limit_name = "pe_start";
		}
		else if ((mda = fid_get_mda_indexed(fid, pvid, ID_LEN, 1)) &&
			 (mdac = mda->metadata_locn)) {
			limit = mdac->area.start;
			limit_name = "MDA1 start";
		}
		else {
			limit = disk_size;
			limit_name = "disk size";
		}

		if (limit > disk_size)
			goto bad;

		mda_start = LABEL_SCAN_SIZE;

		/* Align MDA0 start with page size if possible. */
		if (limit - mda_start >= MDA_SIZE_MIN) {
			if ((adjustment = mda_start % page_size))
				mda_start += (page_size - adjustment);
		}

		/* Align MDA0 end position with given alignment if possible. */
		if (alignment &&
		    (adjustment = (mda_start + mda_size) % alignment)) {
			tmp_mda_size = mda_size + alignment - adjustment;
			if (mda_start + tmp_mda_size <= limit)
				mda_size = tmp_mda_size;
		}

		/* Align MDA0 end position with given alignment offset if possible. */
		if (alignment_offset &&
		    (((mda_start + mda_size) % alignment) == 0)) {
			tmp_mda_size = mda_size + alignment_offset;
			if (mda_start + tmp_mda_size <= limit)
				mda_size = tmp_mda_size;
		}

		if (mda_start + mda_size > limit) {
			/*
			 * Try to decrease the MDA0 size with twice the
			 * alignment and then align with given alignment.
			 * If pe_start is locked, skip this type of
			 * alignment since it would be useless.
			 * Check first whether we can apply that!
			 */
			if (!pe_start_locked &&
			    ((limit - mda_start) > alignment * 2)) {
				mda_size = limit - mda_start - alignment * 2;

				if ((adjustment = (mda_start + mda_size) % alignment))
					mda_size += (alignment - adjustment);

				/* Still too much? Then there's nothing else to do. */
				if (mda_start + mda_size > limit)
					goto bad;
			}
			/* Otherwise, give up and take any usable space. */
			/* FIXME: We should probably check for some minimum MDA size here. */
			else
				mda_size = limit - mda_start;

			limit_applied = 1;
		}

		/*
		 * If PV's pe_start is not locked, update pe_start value with the
		 * start of the area that follows the MDA0 we've just calculated.
		 */
		if (!pe_start_locked) {
			pe_start = mda_start + mda_size;
			pv->pe_start = pe_start >> SECTOR_SHIFT;
		}
	}
	/* Second metadata area at the end of the device. */
	else {
		/*
		 * Try to fit MDA1 start within given pe_end or pe_start limit
		 * if defined or locked. If pe_start is not defined yet, count
		 * with any existing MDA0. If MDA0 does not exist, just use
		 * LABEL_SCAN_SIZE.
		 */
		pe_end = pv->pe_count ? (pv->pe_start +
					 pv->pe_count * pv->pe_size - 1) << SECTOR_SHIFT
				      : 0;

		if (pe_start || pe_start_locked) {
			limit = pe_end ? pe_end : pe_start;
			limit_name = pe_end ? "pe_end" : "pe_start";
		}
		else if ((mda = fid_get_mda_indexed(fid, pvid, ID_LEN, 0)) &&
			 (mdac = mda->metadata_locn)) {
			limit = mdac->area.start + mdac->area.size;
			limit_name = "MDA0 end";
		}
		else {
			limit = LABEL_SCAN_SIZE;
			limit_name = "label scan size";
		}

		if (limit > disk_size)
			goto bad;

		if (mda_size > disk_size) {
			mda_size = disk_size - limit;
			limit_applied = 1;
		}

		mda_start = disk_size - mda_size;

		/* If MDA1 size is too big, just take any usable space. */
		if (disk_size - mda_size < limit) {
			mda_size = disk_size - limit;
			mda_start = disk_size - mda_size;
			limit_applied = 1;
		}
		/* Otherwise, try to align MDA1 start if possible. */
		else if (alignment &&
		    (adjustment = mda_start % alignment)) {
			tmp_mda_size = mda_size + adjustment;
			if (tmp_mda_size < disk_size &&
			    disk_size - tmp_mda_size >= limit) {
				mda_size = tmp_mda_size;
				mda_start = disk_size - mda_size;
			}
		}

		/*
		 * If PV's pe_end not set yet, set it to the end of the
		 * area that precedes the MDA1 we've just calculated.
		 * FIXME: do we need to set this? Isn't it always set before?
		 */
		/*if (!pe_end) {
			pe_end = mda_start;
			pv->pe_end = pe_end >> SECTOR_SHIFT;
		}*/
	}

	if (limit_applied)
		log_very_verbose("Using limited metadata area size on %s "
				 "with value %" PRIu64 " (limited by %s of "
				 "%" PRIu64 ").", pv_dev_name(pv),
				  mda_size, limit_name, limit);

	if (mda_size) {
		/* Wipe metadata area with zeroes. */
		if (!dev_set((struct device *) pv->dev, mda_start,
			(size_t) ((mda_size > wipe_size) ?
				  wipe_size : mda_size), 0)) {
				log_error("Failed to wipe new metadata area "
					  "at the %s of the %s",
					   mda_index ? "end" : "start",
					   pv_dev_name(pv));
				return 0;
		}

		/* Finally, add new metadata area to PV's format instance. */
		if (!_add_metadata_area_to_pv(pv, mda_index, mda_start,
					      mda_size, mda_ignored))
			return_0;
	}

	return 1;

bad:
	log_error("Not enough space available for metadata area "
		  "with index %u on PV %s.", mda_index, pv_dev_name(pv));
	return 0;
}

static int _remove_metadata_area_from_pv(struct physical_volume *pv,
					 unsigned mda_index)
{
	if (mda_index >= FMT_TEXT_MAX_MDAS_PER_PV) {
		log_error(INTERNAL_ERROR "can't remove metadata area with "
					 "index %u from PV %s. Metadata "
					 "layou not supported by %s format.",
					  mda_index, dev_name(pv->dev),
					  pv->fmt->name);
		return 0;
	}

	return fid_remove_mda(pv->fid, NULL, (const char *) &pv->id,
			      ID_LEN, mda_index);
}

static int _text_pv_remove_metadata_area(const struct format_type *fmt,
					 struct physical_volume *pv,
					 unsigned mda_index)
{
	return _remove_metadata_area_from_pv(pv, mda_index);
}

static int _text_pv_resize(const struct format_type *fmt,
			   struct physical_volume *pv,
			   struct volume_group *vg,
			   uint64_t size)
{
	struct format_instance *fid = pv->fid;
	const char *pvid = (const char *) (*pv->old_id.uuid ? &pv->old_id : &pv->id);
	struct metadata_area *mda;
	struct mda_context *mdac;
	uint64_t size_reduction;
	uint64_t mda_size;
	unsigned mda_ignored;

	/*
	 * First, set the new size and update the cache and reset pe_count.
	 * (pe_count must be reset otherwise it would be considered as
	 * a limiting factor while moving the mda!)
	 */
	pv->size = size;
	pv->pe_count = 0;

	/* If there's an mda at the end, move it to a new position. */
	if ((mda = fid_get_mda_indexed(fid, pvid, ID_LEN, 1)) &&
	    (mdac = mda->metadata_locn)) {
		/* FIXME: Maybe MDA0 size would be better? */
		mda_size = mdac->area.size >> SECTOR_SHIFT;
		mda_ignored = mda_is_ignored(mda);

		if (!_text_pv_remove_metadata_area(fmt, pv, 1) ||
		    !_text_pv_add_metadata_area(fmt, pv, 1, 1, mda_size,
						mda_ignored)) {
			log_error("Failed to move metadata area with index 1 "
				  "while resizing PV %s.", pv_dev_name(pv));
			return 0;
		}
	}

	/* If there's a VG, reduce size by counting in pe_start and metadata areas. */
	if (vg) {
		size_reduction = pv_pe_start(pv);
		if ((mda = fid_get_mda_indexed(fid, pvid, ID_LEN, 1)) &&
		    (mdac = mda->metadata_locn))
			size_reduction += mdac->area.size >> SECTOR_SHIFT;
		pv->size -= size_reduction;
	}

	return 1;
}

static struct format_instance *_text_create_text_instance(const struct format_type *fmt,
							  const struct format_instance_ctx *fic)
{
	struct format_instance *fid;

	if (!(fid = alloc_fid(fmt, fic)))
		return_NULL;

	if (_create_vg_text_instance(fid, fic))
		return fid;

	dm_pool_destroy(fid->mem);
	return NULL;
}

static struct format_handler _text_handler = {
	.scan = _text_scan,
	.pv_read = _text_pv_read,
	.pv_initialise = _text_pv_initialise,
	.pv_setup = _text_pv_setup,
	.pv_add_metadata_area = _text_pv_add_metadata_area,
	.pv_remove_metadata_area = _text_pv_remove_metadata_area,
	.pv_resize = _text_pv_resize,
	.pv_write = _text_pv_write,
	.vg_setup = _text_vg_setup,
	.lv_setup = _text_lv_setup,
	.create_instance = _text_create_text_instance,
	.destroy_instance = _text_destroy_instance,
	.destroy = _text_destroy
};

static int _add_dir(const char *dir, struct dm_list *dir_list)
{
	struct dir_list *dl;

	if (dm_create_dir(dir)) {
		if (!(dl = dm_malloc(sizeof(struct dm_list) + strlen(dir) + 1))) {
			log_error("_add_dir allocation failed");
			return 0;
		}
		log_very_verbose("Adding text format metadata dir: %s", dir);
		strcpy(dl->dir, dir);
		dm_list_add(dir_list, &dl->list);
		return 1;
	}

	return 0;
}

static int _get_config_disk_area(struct cmd_context *cmd,
				 const struct dm_config_node *cn, struct dm_list *raw_list)
{
	struct device_area dev_area;
	const char *id_str;
	struct id id;

	if (!(cn = cn->child)) {
		log_error("Empty metadata disk_area section of config file");
		return 0;
	}

	if (!dm_config_get_uint64(cn, "start_sector", &dev_area.start)) {
		log_error("Missing start_sector in metadata disk_area section "
			  "of config file");
		return 0;
	}
	dev_area.start <<= SECTOR_SHIFT;

	if (!dm_config_get_uint64(cn, "size", &dev_area.size)) {
		log_error("Missing size in metadata disk_area section "
			  "of config file");
		return 0;
	}
	dev_area.size <<= SECTOR_SHIFT;

	if (!dm_config_get_str(cn, "id", &id_str)) {
		log_error("Missing uuid in metadata disk_area section "
			  "of config file");
		return 0;
	}

	if (!id_read_format(&id, id_str)) {
		log_error("Invalid uuid in metadata disk_area section "
			  "of config file: %s", id_str);
		return 0;
	}

	if (!(dev_area.dev = lvmcache_device_from_pvid(cmd, &id, NULL, NULL))) {
		char buffer[64] __attribute__((aligned(8)));

		if (!id_write_format(&id, buffer, sizeof(buffer)))
			log_error("Couldn't find device.");
		else
			log_error("Couldn't find device with uuid '%s'.",
				  buffer);

		return 0;
	}

	return _add_raw(raw_list, &dev_area);
}

struct format_type *create_text_format(struct cmd_context *cmd)
{
	struct format_instance_ctx fic;
	struct format_instance *fid;
	struct format_type *fmt;
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	struct mda_lists *mda_lists;

	if (!(fmt = dm_malloc(sizeof(*fmt))))
		return_NULL;

	fmt->cmd = cmd;
	fmt->ops = &_text_handler;
	fmt->name = FMT_TEXT_NAME;
	fmt->alias = FMT_TEXT_ALIAS;
	fmt->orphan_vg_name = ORPHAN_VG_NAME(FMT_TEXT_NAME);
	fmt->features = FMT_SEGMENTS | FMT_MDAS | FMT_TAGS | FMT_PRECOMMIT |
			FMT_UNLIMITED_VOLS | FMT_RESIZE_PV |
			FMT_UNLIMITED_STRIPESIZE;

	if (!(mda_lists = dm_malloc(sizeof(struct mda_lists)))) {
		log_error("Failed to allocate dir_list");
		dm_free(fmt);
		return NULL;
	}

	dm_list_init(&mda_lists->dirs);
	dm_list_init(&mda_lists->raws);
	mda_lists->file_ops = &_metadata_text_file_ops;
	mda_lists->raw_ops = &_metadata_text_raw_ops;
	fmt->private = (void *) mda_lists;

	if (!(fmt->labeller = text_labeller_create(fmt))) {
		log_error("Couldn't create text label handler.");
		goto err;
	}

	if (!(label_register_handler(FMT_TEXT_NAME, fmt->labeller))) {
		log_error("Couldn't register text label handler.");
		fmt->labeller->ops->destroy(fmt->labeller);
		goto err;
	}

	if ((cn = find_config_tree_node(cmd, "metadata/dirs"))) {
		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Invalid string in config file: "
					  "metadata/dirs");
				goto err;
			}

			if (!_add_dir(cv->v.str, &mda_lists->dirs)) {
				log_error("Failed to add %s to text format "
					  "metadata directory list ", cv->v.str);
				goto err;
			}
			cmd->independent_metadata_areas = 1;
		}
	}

	if ((cn = find_config_tree_node(cmd, "metadata/disk_areas"))) {
		for (cn = cn->child; cn; cn = cn->sib) {
			if (!_get_config_disk_area(cmd, cn, &mda_lists->raws))
				goto err;
			cmd->independent_metadata_areas = 1;
		}
	}

	if (!(fmt->orphan_vg = alloc_vg("text_orphan", cmd, fmt->orphan_vg_name))) {
		dm_free(fmt);
		return NULL;
	}

	fic.type = FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = fmt->orphan_vg_name;
	fic.context.vg_ref.vg_id = NULL;
	if (!(fid = _text_create_text_instance(fmt, &fic))) {
		log_error("Failed to create format instance");
		release_vg(fmt->orphan_vg);
		goto err;
	}
	vg_set_fid(fmt->orphan_vg, fid);

	log_very_verbose("Initialised format: %s", fmt->name);

	return fmt;

      err:
	_text_destroy(fmt);

	return NULL;
}
