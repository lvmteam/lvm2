/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc. All rights reserved.
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
#include "lib/format_text/format-text.h"
#include "layout.h"
#include "lib/label/label.h"
#include "lib/mm/xlate.h"
#include "lib/cache/lvmcache.h"

#include <sys/stat.h>
#include <fcntl.h>

static int _text_can_handle(struct labeller *l __attribute__((unused)),
			    void *buf,
			    uint64_t sector __attribute__((unused)))
{
	struct label_header *lh = (struct label_header *) buf;

	if (!memcmp(lh->type, LVM2_LABEL, sizeof(lh->type)))
		return 1;

	return 0;
}

struct _dl_setup_baton {
	struct disk_locn *pvh_dlocn_xl;
	struct device *dev;
};

static int _da_setup(struct disk_locn *da, void *baton)
{
	struct _dl_setup_baton *p = baton;
	p->pvh_dlocn_xl->offset = xlate64(da->offset);
	p->pvh_dlocn_xl->size = xlate64(da->size);
	p->pvh_dlocn_xl++;
	return 1;
}

static int _ba_setup(struct disk_locn *ba, void *baton)
{
	return _da_setup(ba, baton);
}

static int _mda_setup(struct metadata_area *mda, void *baton)
{
	struct _dl_setup_baton *p = baton;
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;

	if (mdac->area.dev != p->dev)
		return 1;

	p->pvh_dlocn_xl->offset = xlate64(mdac->area.start);
	p->pvh_dlocn_xl->size = xlate64(mdac->area.size);
	p->pvh_dlocn_xl++;

	return 1;
}

static int _dl_null_termination(void *baton)
{
	struct _dl_setup_baton *p = baton;

	p->pvh_dlocn_xl->offset = xlate64(UINT64_C(0));
	p->pvh_dlocn_xl->size = xlate64(UINT64_C(0));
	p->pvh_dlocn_xl++;

	return 1;
}

static int _text_write(struct label *label, void *buf)
{
	struct label_header *lh = (struct label_header *) buf;
	struct pv_header *pvhdr;
	struct pv_header_extension *pvhdr_ext;
	struct lvmcache_info *info;
	struct _dl_setup_baton baton;
	char buffer[64] __attribute__((aligned(8)));
	int ba1, da1, mda1, mda2;

	/*
	 * PV header base
	 */
	/* FIXME Move to where label is created */
	memcpy(label->type, LVM2_LABEL, sizeof(label->type));

	memcpy(lh->type, LVM2_LABEL, sizeof(lh->type));

	pvhdr = (struct pv_header *) ((char *) buf + xlate32(lh->offset_xl));
	info = (struct lvmcache_info *) label->info;
	pvhdr->device_size_xl = xlate64(lvmcache_device_size(info));
	memcpy(pvhdr->pv_uuid, &lvmcache_device(info)->pvid, sizeof(struct id));
	if (!id_write_format((const struct id *)pvhdr->pv_uuid, buffer,
			     sizeof(buffer))) {
		stack;
		buffer[0] = '\0';
	}

	baton.dev = lvmcache_device(info);
	baton.pvh_dlocn_xl = &pvhdr->disk_areas_xl[0];

	/* List of data areas (holding PEs) */
	lvmcache_foreach_da(info, _da_setup, &baton);
	_dl_null_termination(&baton);

	/* List of metadata area header locations */
	lvmcache_foreach_mda(info, _mda_setup, &baton);
	_dl_null_termination(&baton);

	/*
	 * PV header extension
	 */
	pvhdr_ext = (struct pv_header_extension *) ((char *) baton.pvh_dlocn_xl);
	pvhdr_ext->version = xlate32(PV_HEADER_EXTENSION_VSN);
	pvhdr_ext->flags = xlate32(lvmcache_ext_flags(info));

	/* List of bootloader area locations */
	baton.pvh_dlocn_xl = &pvhdr_ext->bootloader_areas_xl[0];
	lvmcache_foreach_ba(info, _ba_setup, &baton);
	_dl_null_termination(&baton);

	/* Create debug message with ba, da and mda locations */
	ba1 = (xlate64(pvhdr_ext->bootloader_areas_xl[0].offset) ||
	       xlate64(pvhdr_ext->bootloader_areas_xl[0].size)) ? 0 : -1;

	da1 = (xlate64(pvhdr->disk_areas_xl[0].offset) ||
	       xlate64(pvhdr->disk_areas_xl[0].size)) ? 0 : -1;

	mda1 = da1 + 2;
	mda2 = mda1 + 1;
	
	if (!xlate64(pvhdr->disk_areas_xl[mda1].offset) &&
	    !xlate64(pvhdr->disk_areas_xl[mda1].size))
		mda1 = mda2 = 0;
	else if (!xlate64(pvhdr->disk_areas_xl[mda2].offset) &&
		 !xlate64(pvhdr->disk_areas_xl[mda2].size))
		mda2 = 0;

	log_debug_metadata("%s: Preparing PV label header %s size " FMTu64 " with"
			   "%s%.*" PRIu64 "%s%.*" PRIu64 "%s"
			   "%s%.*" PRIu64 "%s%.*" PRIu64 "%s"
			   "%s%.*" PRIu64 "%s%.*" PRIu64 "%s"
			   "%s%.*" PRIu64 "%s%.*" PRIu64 "%s",
			   dev_name(lvmcache_device(info)), buffer, lvmcache_device_size(info),
			   (ba1 > -1) ? " ba1 (" : "",
			   (ba1 > -1) ? 1 : 0,
			   (ba1 > -1) ? xlate64(pvhdr_ext->bootloader_areas_xl[ba1].offset) >> SECTOR_SHIFT : 0,
			   (ba1 > -1) ? "s, " : "",
			   (ba1 > -1) ? 1 : 0,
			   (ba1 > -1) ? xlate64(pvhdr_ext->bootloader_areas_xl[ba1].size) >> SECTOR_SHIFT : 0,
			   (ba1 > -1) ? "s)" : "",
			   (da1 > -1) ? " da1 (" : "",
			   (da1 > -1) ? 1 : 0,
			   (da1 > -1) ? xlate64(pvhdr->disk_areas_xl[da1].offset) >> SECTOR_SHIFT : 0,
			   (da1 > -1) ? "s, " : "",
			   (da1 > -1) ? 1 : 0,
			   (da1 > -1) ? xlate64(pvhdr->disk_areas_xl[da1].size) >> SECTOR_SHIFT : 0,
			   (da1 > -1) ? "s)" : "",
			   mda1 ? " mda1 (" : "",
			   mda1 ? 1 : 0,
			   mda1 ? xlate64(pvhdr->disk_areas_xl[mda1].offset) >> SECTOR_SHIFT : 0,
			   mda1 ? "s, " : "",
			   mda1 ? 1 : 0,
			   mda1 ? xlate64(pvhdr->disk_areas_xl[mda1].size) >> SECTOR_SHIFT : 0,
			   mda1 ? "s)" : "",
			   mda2 ? " mda2 (" : "",
			   mda2 ? 1 : 0,
			   mda2 ? xlate64(pvhdr->disk_areas_xl[mda2].offset) >> SECTOR_SHIFT : 0,
			   mda2 ? "s, " : "",
			   mda2 ? 1 : 0,
			   mda2 ? xlate64(pvhdr->disk_areas_xl[mda2].size) >> SECTOR_SHIFT : 0,
			   mda2 ? "s)" : "");

	if (da1 < 0) {
		log_error(INTERNAL_ERROR "%s label header currently requires "
			  "a data area.", dev_name(lvmcache_device(info)));
		return 0;
	}

	return 1;
}

int add_da(struct dm_pool *mem, struct dm_list *das,
	   uint64_t start, uint64_t size)
{
	struct data_area_list *dal;

	if (!mem) {
		if (!(dal = malloc(sizeof(*dal)))) {
			log_error("struct data_area_list allocation failed");
			return 0;
		}
	} else {
		if (!(dal = dm_pool_alloc(mem, sizeof(*dal)))) {
			log_error("struct data_area_list allocation failed");
			return 0;
		}
	}

	dal->disk_locn.offset = start;
	dal->disk_locn.size = size;

	dm_list_add(das, &dal->list);

	return 1;
}

void del_das(struct dm_list *das)
{
	struct dm_list *dah, *tmp;
	struct data_area_list *da;

	dm_list_iterate_safe(dah, tmp, das) {
		da = dm_list_item(dah, struct data_area_list);
		dm_list_del(&da->list);
		free(da);
	}
}

int add_ba(struct dm_pool *mem, struct dm_list *eas,
	   uint64_t start, uint64_t size)
{
	return add_da(mem, eas, start, size);
}

void del_bas(struct dm_list *bas)
{
	del_das(bas);
}

int add_mda(const struct format_type *fmt, struct dm_pool *mem, struct dm_list *mdas,
	    struct device *dev, uint64_t start, uint64_t size, unsigned ignored,
	    struct metadata_area **mda_new)
{
	struct metadata_area *mdal, *mda;
	struct mda_lists *mda_lists = (struct mda_lists *) fmt->private;
	struct mda_context *mdac, *mdac2;

	if (!mem) {
		if (!(mdal = malloc(sizeof(struct metadata_area)))) {
			log_error("struct mda_list allocation failed");
			return 0;
		}

		if (!(mdac = malloc(sizeof(struct mda_context)))) {
			log_error("struct mda_context allocation failed");
			free(mdal);
			return 0;
		}
	} else {
		if (!(mdal = dm_pool_alloc(mem, sizeof(struct metadata_area)))) {
			log_error("struct mda_list allocation failed");
			return 0;
		}

		if (!(mdac = dm_pool_alloc(mem, sizeof(struct mda_context)))) {
			log_error("struct mda_context allocation failed");
			return 0;
		}
	}

	mdal->ops = mda_lists->raw_ops;
	mdal->metadata_locn = mdac;

	mdac->area.dev = dev;
	mdac->area.start = start;
	mdac->area.size = size;
	mdac->free_sectors = UINT64_C(0);
	memset(&mdac->rlocn, 0, sizeof(mdac->rlocn));

	/* Set MDA_PRIMARY only if this is the first metadata area on this device. */
	mdal->status = MDA_PRIMARY;
	dm_list_iterate_items(mda, mdas) {
		mdac2 = mda->metadata_locn;
		if (mdac2->area.dev == dev) {
			mdal->status = 0;
			break;
		}
	}

	mda_set_ignored(mdal, ignored);

	dm_list_add(mdas, &mdal->list);
	if (mda_new)
		*mda_new = mdal;
	return 1;
}

void del_mdas(struct dm_list *mdas)
{
	struct dm_list *mdah, *tmp;
	struct metadata_area *mda;

	dm_list_iterate_safe(mdah, tmp, mdas) {
		mda = dm_list_item(mdah, struct metadata_area);
		free(mda->metadata_locn);
		dm_list_del(&mda->list);
		free(mda);
	}
}

static int _text_initialise_label(struct labeller *l __attribute__((unused)),
				  struct label *label)
{
	memcpy(label->type, LVM2_LABEL, sizeof(label->type));

	return 1;
}

static int _read_mda_header_and_metadata(const struct format_type *fmt,
					 struct metadata_area *mda,
					 struct lvmcache_vgsummary *vgsummary,
					 uint32_t *bad_fields)
{
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct mda_header *mdah;

	if (!(mdah = raw_read_mda_header(fmt, &mdac->area, (mda->mda_num == 1), 0, bad_fields))) {
		log_warn("WARNING: bad metadata header on %s at %llu.",
			 dev_name(mdac->area.dev),
			 (unsigned long long)mdac->area.start);
		mda->header_start = mdac->area.start;
		*bad_fields |= BAD_MDA_HEADER;
		return 0;
	}

	mda->header_start = mdah->start;

	mda_set_ignored(mda, rlocn_is_ignored(mdah->raw_locns));

	if (mda_is_ignored(mda)) {
		log_debug_metadata("Ignoring mda on device %s at offset " FMTu64,
				   dev_name(mdac->area.dev),
				   mdac->area.start);
		vgsummary->mda_ignored = 1;
		return 1;
	}

	if (!read_metadata_location_summary(fmt, mda, mdah, mda_is_primary(mda), &mdac->area,
					    vgsummary, &mdac->free_sectors)) {
		if (vgsummary->zero_offset)
			return 1;

		log_warn("WARNING: bad metadata text on %s in mda%d",
			 dev_name(mdac->area.dev), mda->mda_num);
		*bad_fields |= BAD_MDA_TEXT;
		return 0;
	}

	return 1;
}

/*
 * Used by label_scan to get a summary of the VG that exists on this PV.  This
 * summary is stored in lvmcache vginfo/info/info->mdas and is used later by
 * vg_read which needs to know which PVs to read for a given VG name, and where
 * the metadata is at for those PVs.
 */

static int _text_read(struct labeller *labeller, struct device *dev, void *label_buf,
		      uint64_t label_sector, int *is_duplicate)
{
	struct lvmcache_vgsummary vgsummary;
	struct lvmcache_info *info;
	const struct format_type *fmt = labeller->fmt;
	struct label_header *lh = (struct label_header *) label_buf;
	struct pv_header *pvhdr;
	struct pv_header_extension *pvhdr_ext;
	struct metadata_area *mda;
	struct metadata_area *mda1 = NULL;
	struct metadata_area *mda2 = NULL;
	struct disk_locn *dlocn_xl;
	uint64_t offset;
	uint32_t ext_version;
	uint32_t bad_fields;
	int mda_count = 0;
	int good_mda_count = 0;
	int bad_mda_count = 0;
	int rv1, rv2;

	/*
	 * PV header base
	 */
	pvhdr = (struct pv_header *) ((char *) label_buf + xlate32(lh->offset_xl));

	/*
	 * FIXME: stop adding the device to lvmcache initially as an orphan
	 * (and then moving it later) and instead just add it when we know the
	 * VG.
	 *
	 * If another device with this same PVID has already been seen,
	 * lvmcache_add will put this device in the duplicates list in lvmcache
	 * and return NULL.  At the end of label_scan, the duplicate devs are
	 * compared, and if another dev is preferred for this PV, then the
	 * existing dev is removed from lvmcache and _text_read is called again
	 * for this dev, and lvmcache_add will add it.
	 *
	 * Other reasons for lvmcache_add to return NULL are internal errors.
	 */
	if (!(info = lvmcache_add(labeller, (char *)pvhdr->pv_uuid, dev, label_sector,
				  FMT_TEXT_ORPHAN_VG_NAME,
				  FMT_TEXT_ORPHAN_VG_NAME, 0, is_duplicate)))
		return_0;

	lvmcache_set_device_size(info, xlate64(pvhdr->device_size_xl));

	lvmcache_del_das(info);
	lvmcache_del_mdas(info);
	lvmcache_del_bas(info);

	/* Data areas holding the PEs */
	dlocn_xl = pvhdr->disk_areas_xl;
	while ((offset = xlate64(dlocn_xl->offset))) {
		lvmcache_add_da(info, offset, xlate64(dlocn_xl->size));
		dlocn_xl++;
	}

	dlocn_xl++;

	/* Metadata areas */
	while ((offset = xlate64(dlocn_xl->offset))) {

		/*
		 * This just calls add_mda() above, replacing info with info->mdas.
		 */
		lvmcache_add_mda(info, dev, offset, xlate64(dlocn_xl->size), 0, &mda);

		dlocn_xl++;
		mda_count++;

		if (mda_count == 1) {
			mda1 = mda;
			mda1->mda_num = 1;
		}
		else if (mda_count == 2) {
			mda2 = mda;
			mda2->mda_num = 2;
		}
	}

	dlocn_xl++;

	/*
	 * PV header extension
	 */
	pvhdr_ext = (struct pv_header_extension *) ((char *) dlocn_xl);
	if (!(ext_version = xlate32(pvhdr_ext->version)))
		goto scan_mdas;

	log_debug_metadata("%s: PV header extension version " FMTu32 " found",
			   dev_name(dev), ext_version);

	/* Extension version */
	lvmcache_set_ext_version(info, xlate32(pvhdr_ext->version));

	/* Extension flags */
	lvmcache_set_ext_flags(info, xlate32(pvhdr_ext->flags));

	/* Bootloader areas */
	dlocn_xl = pvhdr_ext->bootloader_areas_xl;
	while ((offset = xlate64(dlocn_xl->offset))) {
		lvmcache_add_ba(info, offset, xlate64(dlocn_xl->size));
		dlocn_xl++;
	}

 scan_mdas:
	if (!mda_count) {
		log_debug_metadata("Scanning %s found no mdas.", dev_name(dev));
		return 1;
	}

	/*
	 * Track which devs have bad metadata so repair can find them (even if
	 * this dev also has good metadata that we are able to use).
	 *
	 * When bad metadata is seen, the unusable mda struct is removed from
	 * lvmcache info->mdas.  This means that vg_read and vg_write will skip
	 * the bad mda not try to read or write the bad metadata.  The bad mdas
	 * are saved in a separate bad_mdas list in lvmcache so that repair can
	 * find them to repair.
	 */

	if (mda1) {
		log_debug_metadata("Scanning %s mda1 summary.", dev_name(dev));
		memset(&vgsummary, 0, sizeof(vgsummary));
		dm_list_init(&vgsummary.pvsummaries);
		bad_fields = 0;
		vgsummary.mda_num = 1;

		rv1 = _read_mda_header_and_metadata(fmt, mda1, &vgsummary, &bad_fields);

		if (rv1 && !vgsummary.zero_offset && !vgsummary.mda_ignored) {
			if (!lvmcache_update_vgname_and_id(info, &vgsummary)) {
				/* I believe this is only an internal error. */

				dm_list_del(&mda1->list);

				/* Are there other cases besides mismatch and internal error? */
				if (vgsummary.mismatch) {
					log_warn("WARNING: Scanning %s mda1 found mismatch with other metadata.", dev_name(dev));
					bad_fields |= BAD_MDA_MISMATCH;
				} else {
					log_warn("WARNING: Scanning %s mda1 failed to save internal summary.", dev_name(dev));
					bad_fields |= BAD_MDA_INTERNAL;
				}
				mda1->bad_fields = bad_fields;
				lvmcache_save_bad_mda(info, mda1);
				mda1 = NULL;
				bad_mda_count++;
			} else {
				/* The normal success path */
				log_debug("Scanned %s mda1 seqno %u", dev_name(dev), vgsummary.seqno);
				good_mda_count++;
			}
		}

		if (!rv1) {
			/*
			 * Remove the bad mda from normal mda list so it's not
			 * used by vg_read/vg_write, but keep track of it in
			 * lvmcache for repair.
			 */
			log_warn("WARNING: scanning %s mda1 failed to read metadata summary.", dev_name(dev));
			log_warn("WARNING: repair VG metadata on %s with vgck --updatemetadata.", dev_name(dev));

			dm_list_del(&mda1->list);
			mda1->bad_fields = bad_fields;
			lvmcache_save_bad_mda(info, mda1);
			mda1 = NULL;
			bad_mda_count++;
		}
	}

	if (mda2) {
		log_debug_metadata("Scanning %s mda2 summary.", dev_name(dev));
		memset(&vgsummary, 0, sizeof(vgsummary));
		dm_list_init(&vgsummary.pvsummaries);
		bad_fields = 0;
		vgsummary.mda_num = 2;

		rv2 = _read_mda_header_and_metadata(fmt, mda2, &vgsummary, &bad_fields);

		if (rv2 && !vgsummary.zero_offset && !vgsummary.mda_ignored) {
			if (!lvmcache_update_vgname_and_id(info, &vgsummary)) {
				dm_list_del(&mda2->list);

				/* Are there other cases besides mismatch and internal error? */
				if (vgsummary.mismatch) {
					log_warn("WARNING: Scanning %s mda2 found mismatch with other metadata.", dev_name(dev));
					bad_fields |= BAD_MDA_MISMATCH;
				} else {
					log_warn("WARNING: Scanning %s mda2 failed to save internal summary.", dev_name(dev));
					bad_fields |= BAD_MDA_INTERNAL;
				}

				mda2->bad_fields = bad_fields;
				lvmcache_save_bad_mda(info, mda2);
				mda2 = NULL;
				bad_mda_count++;
			} else {
				/* The normal success path */
				log_debug("Scanned %s mda2 seqno %u", dev_name(dev), vgsummary.seqno);
				good_mda_count++;
			}
		}

		if (!rv2) {
			/*
			 * Remove the bad mda from normal mda list so it's not
			 * used by vg_read/vg_write, but keep track of it in
			 * lvmcache for repair.
			 */
			log_warn("WARNING: scanning %s mda2 failed to read metadata summary.", dev_name(dev));
			log_warn("WARNING: repair VG metadata on %s with vgck --updatemetadata.", dev_name(dev));

			dm_list_del(&mda2->list);
			mda2->bad_fields = bad_fields;
			lvmcache_save_bad_mda(info, mda2);
			mda2 = NULL;
			bad_mda_count++;
		}
	}

	if (good_mda_count)
		return 1;

	if (bad_mda_count)
		return 0;

	/* no metadata in the mdas */
	return 1;
}

static void _text_destroy_label(struct labeller *l __attribute__((unused)),
				struct label *label)
{
	struct lvmcache_info *info = (struct lvmcache_info *) label->info;

	lvmcache_del_mdas(info);
	lvmcache_del_das(info);
	lvmcache_del_bas(info);
}

static void _fmt_text_destroy(struct labeller *l)
{
	free(l);
}

struct label_ops _text_ops = {
	.can_handle = _text_can_handle,
	.write = _text_write,
	.read = _text_read,
	.initialise_label = _text_initialise_label,
	.destroy_label = _text_destroy_label,
	.destroy = _fmt_text_destroy,
};

struct labeller *text_labeller_create(const struct format_type *fmt)
{
	struct labeller *l;

	if (!(l = zalloc(sizeof(*l)))) {
		log_error("Couldn't allocate labeller object.");
		return NULL;
	}

	l->ops = &_text_ops;
	l->fmt = fmt;

	return l;
}
