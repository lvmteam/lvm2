/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2007 Red Hat, Inc. All rights reserved.
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
#include "tools.h"
#include "lib/format_text/format-text.h"
#include "lib/format_text/layout.h"
#include "lib/mm/xlate.h"
#include "lib/misc/crc.h"

static char *_chars_to_str(void *in, void *out, int num, int max, const char *field)
{
	char *i = in;
	char *o = out;
	int n;

	memset(out, 0, max);

	if (num > max-1) {
		log_print("CHECK: abbreviating output for %s", field);
		num = max - 1;
	}

	for (n = 0; n < num; n++) {
		if (isprint((int)*i))
			*o = *i;
		else
			*o = '?';
		i++;
		o++;
	}

	return out;
}

/* 
 * This is used to print mda_header.magic as a series of hex values
 * since it only contains some printable chars.
 */
static char *_chars_to_hexstr(void *in, void *out, int num, int max, const char *field)
{
	char *tmp;
	char *i = in;
	int n;
	int off = 0;
	int ret;

	if (!(tmp = zalloc(max))) {
		log_print("CHECK: no mem for printing %s", field);
		return out;
	}

	memset(out, 0, max);
	memset(tmp, 0, max);

	if (num > max-1) {
		log_print("CHECK: abbreviating output for %s", field);
		num = max - 1;
	}

	for (n = 0; n < num; n++) {
		ret = sprintf(tmp+off, "%x", *i & 0xFF);
		off += ret;
		i++;
	}

	memcpy(out, tmp, 256);

	free(tmp);

	return out;
}

static int _check_label_header(struct label_header *lh, uint64_t labelsector)
{
	uint32_t crc;
	int bad = 0;

	if (memcmp(lh->id, LABEL_ID, sizeof(lh->id))) {
		log_print("CHECK: label_header.id expected %s", LABEL_ID);
		bad++;
	}

	if (xlate64(lh->sector_xl) != labelsector) {
		log_print("CHECK: label_header.sector expected %d", (int)labelsector);
		bad++;
	}

	crc = calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl,
		       LABEL_SIZE - ((uint8_t *) &lh->offset_xl - (uint8_t *) lh));
		
	if (crc != xlate32(lh->crc_xl)) {
		log_print("CHECK: label_header.crc expected 0x%x", crc);
		bad++;
	}

	if (xlate32(lh->offset_xl) != 32) {
		log_print("CHECK: label_header.offset expected 32");
		bad++;
	}

	if (memcmp(lh->type, LVM2_LABEL, sizeof(lh->type))) {
		log_print("CHECK: label_header.type expected %s", LVM2_LABEL);
		bad++;
	}

	if (bad)
		return 0;
	return 1;
}

static int _check_pv_header(struct pv_header *ph)
{
	struct id id;
	int bad = 0;

	if (!id_read_format_try(&id, (char *)&ph->pv_uuid)) {
		log_print("CHECK: pv_header.pv_uuid invalid format");
		bad++;
	}

	if (bad)
		return 0;
	return 1;
}

/*
 * mda_offset/mda_size are from the pv_header/disk_locn and could
 * be incorrect.
 */
static int _check_mda_header(struct mda_header *mh, int mda_num, uint64_t mda_offset, uint64_t mda_size)
{
	char str[256];
	uint32_t crc;
	int bad = 0;

	crc = calc_crc(INITIAL_CRC, (uint8_t *)mh->magic,
		       MDA_HEADER_SIZE - sizeof(mh->checksum_xl));

	if (crc != xlate32(mh->checksum_xl)) {
		log_print("CHECK: mda_header_%d.checksum expected 0x%x", mda_num, crc);
		bad++;
	}

	if (memcmp(mh->magic, FMTT_MAGIC, sizeof(mh->magic))) {
		log_print("CHECK: mda_header_%d.magic expected 0x%s", mda_num, _chars_to_hexstr((void *)&FMTT_MAGIC, str, 16, 256, "mda_header.magic"));
		bad++;
	}

	if (xlate32(mh->version) != FMTT_VERSION) {
		log_print("CHECK: mda_header_%d.version expected %u", mda_num, FMTT_VERSION);
		bad++;
	}

	if (xlate64(mh->start) != mda_offset) {
		log_print("CHECK: mda_header_%d.start does not match pv_header.disk_locn.offset %llu", mda_num, (unsigned long long)mda_offset);
		bad++;
	}

	if (xlate64(mh->size) != mda_size) {
		log_print("CHECK: mda_header_%d.size does not match pv_header.disk_locn.size %llu", mda_num, (unsigned long long)mda_size);
		bad++;
	}

	if (bad)
		return 0;
	return 1;
}

/*
 *
 * mda_offset, mda_size are from pv_header.disk_locn
 * (the location of the metadata area.)
 *
 * meta_offset, meta_size, meta_checksum are from mda_header.raw_locn
 * (the location of the metadata text in the metadata area.)
 */

static int _dump_raw_locn(struct device *dev, int print_fields,
			  struct raw_locn *rlocn, int rlocn_index, uint64_t rlocn_offset,
			  int mda_num, uint64_t mda_offset, uint64_t mda_size,
			  uint64_t *meta_offset_ret,
			  uint64_t *meta_size_ret,
			  uint32_t *meta_checksum_ret)
{
	uint64_t meta_offset, meta_size;
	uint32_t meta_checksum;
	uint32_t meta_flags;
	int bad = 0;
	int mn = mda_num; /* 1 or 2 */
	int ri = rlocn_index; /* 0 or 1 */
	int wrapped = 0;

	meta_offset = xlate64(rlocn->offset);
	meta_size = xlate64(rlocn->size);
	meta_checksum = xlate32(rlocn->checksum);
	meta_flags = xlate32(rlocn->flags);

	if (meta_offset + meta_size > mda_size)
		wrapped = 1;

	if (print_fields) {
		log_print("mda_header_%d.raw_locn[%d] at %llu # %s%s", mn, ri, (unsigned long long)rlocn_offset, (ri == 0) ? "commit" : "precommit", wrapped ? " wrapped" : "");
		log_print("mda_header_%d.raw_locn[%d].offset %llu", mn, ri, (unsigned long long)meta_offset);
		log_print("mda_header_%d.raw_locn[%d].size %llu", mn, ri, (unsigned long long)meta_size);
		log_print("mda_header_%d.raw_locn[%d].checksum 0x%x", mn, ri, meta_checksum);

		if (meta_flags & RAW_LOCN_IGNORED)
			log_print("mda_header_%d.raw_locn[%d].flags 0x%x # RAW_LOCN_IGNORED", mn, ri, meta_flags);
		else
			log_print("mda_header_%d.raw_locn[%d].flags 0x%x", mn, ri, meta_flags);
	}

	/* The precommit pointer will usually be empty. */
	if ((rlocn_index == 1) && meta_offset)
		log_print("CHECK: mda_header_%d.raw_locn[%d] for precommit not empty", mn, ri);

	/* This metadata area is not being used to hold text metadata. */
	/* Old, out of date text metadata may exist if the area was once used. */
	if (meta_flags & RAW_LOCN_IGNORED)
		return 1;

	/*
	 * A valid meta_size can be no larger than the metadata area size minus
	 * the 512 bytes used by the mda_header sector.
	 */
	if (meta_size > (mda_size - 512)) {
		log_print("CHECK: mda_header_%d.raw_locn[%d].size larger than metadata area size", mn, ri);
		/* If meta_size is bad, try to continue using a reasonable value */
		meta_size = (mda_size - 512);
	}

	if (meta_offset_ret)
		*meta_offset_ret = meta_offset;
	if (meta_size_ret)
		*meta_size_ret = meta_size;
	if (meta_checksum_ret)
		*meta_checksum_ret = meta_checksum;

	/* No text metadata exists in this metadata area. */
	if (!meta_offset)
		return 1;

	if (bad)
		return 0;
	return 1;
}

static int _dump_meta_area(struct device *dev, const char *tofile,
			   uint64_t mda_offset, uint64_t mda_size)
{
	FILE *fp;
	char *meta_buf;

	if (!tofile)
		return_0;

	if (!(meta_buf = malloc(mda_size)))
		return_0;
	memset(meta_buf, 0, mda_size);

	if (!dev_read_bytes(dev, mda_offset, mda_size, meta_buf)) {
		log_print("CHECK: failed to read metadata area at offset %llu size %llu",
			  (unsigned long long)mda_offset, (unsigned long long)mda_size);
		return 0;
	}

	if (!(fp = fopen(tofile, "wx"))) {
		log_error("Failed to create file %s", tofile);
		return 0;
	}

	fwrite(meta_buf, mda_size - 512, 1, fp);

	if (fflush(fp))
		stack;
	if (fclose(fp))
		stack;
	return 1;
}

static int _dump_meta_text(struct device *dev,
			   int print_fields, int print_metadata, const char *tofile,
			   int mda_num, int rlocn_index,
			   uint64_t mda_offset, uint64_t mda_size,
			   uint64_t meta_offset, uint64_t meta_size,
			   uint32_t meta_checksum)
{
	char *meta_buf;
	struct dm_config_tree *cft;
	const char *vgname = NULL;
	uint32_t crc;
	int mn = mda_num; /* 1 or 2 */
	int ri = rlocn_index; /* 0 or 1 */
	int bad = 0;

	if (!(meta_buf = malloc(meta_size))) {
		log_print("CHECK: mda_header_%d.raw_locn[%d] no mem for metadata text size %llu", mn, ri,
			  (unsigned long long)meta_size);
		return 0;
	}
	memset(meta_buf, 0, meta_size);

	/*
	 * Read the metadata text specified by the raw_locn so we can
	 * check the raw_locn values.
	 *
	 * meta_offset is the offset from the start of the mda_header,
	 * so the text location from the start of the disk is
	 * mda_offset + meta_offset.
	 */
	if (meta_offset + meta_size > mda_size) {
	 	/* text metadata wraps to start of text metadata area */
		uint32_t wrap = (uint32_t) ((meta_offset + meta_size) - mda_size);
		off_t offset_a = mda_offset + meta_offset;
		uint32_t size_a = meta_size - wrap;
		off_t offset_b = mda_offset + 512; /* continues after mda_header sector */
		uint32_t size_b = wrap;

		if (!dev_read_bytes(dev, offset_a, size_a, meta_buf)) {
			log_print("CHECK: failed to read metadata text at mda_header_%d.raw_locn[%d].offset %llu size %llu part_a %llu %llu", mn, ri,
				  (unsigned long long)meta_offset, (unsigned long long)meta_size,
				  (unsigned long long)offset_a, (unsigned long long)size_a);
			return 0;
		}

		if (!dev_read_bytes(dev, offset_b, size_b, meta_buf + size_a)) {
			log_print("CHECK: failed to read metadata text at mda_header_%d.raw_locn[%d].offset %llu size %llu part_b %llu %llu", mn, ri,
				  (unsigned long long)meta_offset, (unsigned long long)meta_size,
				  (unsigned long long)offset_b, (unsigned long long)size_b);
			return 0;
		}
	} else {
		if (!dev_read_bytes(dev, mda_offset + meta_offset, meta_size, meta_buf)) {
			log_print("CHECK: failed to read metadata text at mda_header_%d.raw_locn[%d].offset %llu size %llu", mn, ri,
				  (unsigned long long)meta_offset, (unsigned long long)meta_size);
			return 0;
		}
	}

	crc = calc_crc(INITIAL_CRC, (uint8_t *)meta_buf, meta_size);
	if (crc != meta_checksum) {
		log_print("CHECK: metadata text at %llu crc does not match mda_header_%d.raw_locn[%d].checksum",
			  (unsigned long long)(mda_offset + meta_offset), mn, ri);
		bad++;
	}

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, NULL, 0))) {
		log_print("CHECK: failed to set up metadata parsing");
		bad++;
	} else {
		if (!dm_config_parse(cft, meta_buf, meta_buf + meta_size)) {
			log_print("CHECK: failed to parse metadata text at %llu size %llu",
				  (unsigned long long)(mda_offset + meta_offset),
				   (unsigned long long)meta_size);
			bad++;
		} else {
			vgname = strdup(cft->root->key);
		}
		config_destroy(cft);
	}

	log_print("metadata text at %llu crc 0x%x # vgname %s",
		  (unsigned long long)(mda_offset + meta_offset), crc,
		  vgname ? vgname : "?");

	if (!print_metadata)
		goto out;

	if (!tofile) {
		log_print("---");
		printf("%s\n", meta_buf);
		log_print("---");
	} else {
		FILE *fp;
		if (!(fp = fopen(tofile, "wx"))) {
			log_error("Failed to create file %s", tofile);
			goto out;
		}

		fprintf(fp, "%s", meta_buf);

		if (fflush(fp))
			stack;
		if (fclose(fp))
			stack;
	}

 out:
	if (bad)
		return 0;
	return 1;
}

static int _dump_label_and_pv_header(struct cmd_context *cmd, int print_fields,
				     struct device *dev,
				     uint64_t *mda1_offset, uint64_t *mda1_size,
				     uint64_t *mda2_offset, uint64_t *mda2_size)
{
	char str[256];
	struct label_header *lh;
	struct pv_header *pvh;
	struct pv_header_extension *pvhe;
	struct disk_locn *dlocn;
	uint64_t lh_offset;
	uint64_t pvh_offset;
	uint64_t pvhe_offset;
	uint64_t dlocn_offset;
	char *buf;
	uint64_t labelsector;
	uint64_t tmp;
	int mda_count = 0;
	int bad = 0;
	int di;

	/*
	 * By default LVM skips the first sector (sector 0), and writes
	 * the label_header in the second sector (sector 1).
	 * (sector size 512 bytes)
	 */
	if (arg_is_set(cmd, labelsector_ARG))
		labelsector = arg_uint64_value(cmd, labelsector_ARG, UINT64_C(0));
	else
		labelsector = 1;

	lh_offset = labelsector * 512; /* from start of disk */

	if (!(buf = zalloc(512)))
		return_0;

	if (!dev_read_bytes(dev, lh_offset, 512, buf)) {
		log_print("CHECK: failed to read label_header at %llu",
			  (unsigned long long)lh_offset);
		return 0;
	}

	lh = (struct label_header *)buf;

	if (print_fields) {
		log_print("label_header at %llu", (unsigned long long)lh_offset);
		log_print("label_header.id %s", _chars_to_str(lh->id, str, 8, 256, "label_header.id"));
		log_print("label_header.sector %llu", (unsigned long long)xlate64(lh->sector_xl));
		log_print("label_header.crc 0x%x", xlate32(lh->crc_xl));
		log_print("label_header.offset %u", xlate32(lh->offset_xl));
		log_print("label_header.type %s", _chars_to_str(lh->type, str, 8, 256, "label_header.type"));
	}

	if (!_check_label_header(lh, labelsector))
		bad++;

	/*
	 * The label_header is 32 bytes in size (size of struct label_header).
	 * The pv_header should begin immediately after the label_header.
	 * The label_header.offset gives the offset of pv_header from the
	 * start of the label_header, which should always be 32.
	 *
	 * If label_header.offset is corrupted, then we should print a
	 * warning about the bad value, and read the pv_header from the
	 * correct location instead of the bogus location.
	 */

	pvh = (struct pv_header *)(buf + 32);
	pvh_offset = lh_offset + 32; /* from start of disk */

	/* sanity check */
	if ((void *)pvh != (void *)(buf + pvh_offset - lh_offset))
		log_print("CHECK: problem with pv_header offset calculation");

	if (print_fields) {
		log_print("pv_header at %llu", (unsigned long long)pvh_offset);
		log_print("pv_header.pv_uuid %s", _chars_to_str(pvh->pv_uuid, str, ID_LEN, 256, "pv_header.pv_uuid"));
		log_print("pv_header.device_size %llu", (unsigned long long)xlate64(pvh->device_size_xl));
	}

	if (!_check_pv_header(pvh))
		bad++;

	/*
	 * The pv_header is 40 bytes, excluding disk_locn's.
	 * disk_locn structs immediately follow the pv_header.
	 * Each disk_locn is 16 bytes.
	 */
	di = 0;
	dlocn = pvh->disk_areas_xl;
	dlocn_offset = pvh_offset + 40; /* from start of disk */

	/* sanity check */
	if ((void *)dlocn != (void *)(buf + dlocn_offset - lh_offset))
		log_print("CHECK: problem with pv_header.disk_locn[%d] offset calculation", di);

	while ((tmp = xlate64(dlocn->offset))) {
		if (print_fields) {
			log_print("pv_header.disk_locn[%d] at %llu # location of data area", di,
				  (unsigned long long)dlocn_offset);
			log_print("pv_header.disk_locn[%d].offset %llu", di,
				  (unsigned long long)xlate64(dlocn->offset));
			log_print("pv_header.disk_locn[%d].size %llu", di,
				  (unsigned long long)xlate64(dlocn->size));
		}
		di++;
		dlocn++;
		dlocn_offset += 16;
	}

	/* all-zero dlocn struct is area list end */
	if (print_fields) {
		log_print("pv_header.disk_locn[%d] at %llu # location list end", di,
			  (unsigned long long) dlocn_offset);
		log_print("pv_header.disk_locn[%d].offset %llu", di,
			  (unsigned long long)xlate64(dlocn->offset));
		log_print("pv_header.disk_locn[%d].size %llu", di,
			  (unsigned long long)xlate64(dlocn->size));
	}

	/* advance past the all-zero dlocn struct */
	di++;
	dlocn++;
	dlocn_offset += 16;

	/* sanity check */
	if ((void *)dlocn != (void *)(buf + dlocn_offset - lh_offset))
		log_print("CHECK: problem with pv_header.disk_locn[%d] offset calculation", di);

	while ((tmp = xlate64(dlocn->offset))) {
		if (print_fields) {
			log_print("pv_header.disk_locn[%d] at %llu # location of metadata area", di,
				  (unsigned long long)dlocn_offset);
			log_print("pv_header.disk_locn[%d].offset %llu", di,
				  (unsigned long long)xlate64(dlocn->offset));
			log_print("pv_header.disk_locn[%d].size %llu", di,
				  (unsigned long long)xlate64(dlocn->size));
		}

		if (!mda_count) {
			*mda1_offset = xlate64(dlocn->offset);
			*mda1_size = xlate64(dlocn->size);

			if (*mda1_offset != 4096) {
				log_print("CHECK: pv_header.disk_locn[%d].offset expected 4096 # for first mda", di);
				bad++;
			}
		} else {
			*mda2_offset = xlate64(dlocn->offset);
			*mda2_size = xlate64(dlocn->size);

			/*
			 * No fixed location for second mda, so we have to look for
			 * mda_header at this offset to see if it's correct.
			 */
		}

		di++;
		dlocn++;
		dlocn_offset += 16;
		mda_count++;
	}

	/* all-zero dlocn struct is area list end */
	if (print_fields) {
		log_print("pv_header.disk_locn[%d] at %llu # location list end", di,
			  (unsigned long long) dlocn_offset);
		log_print("pv_header.disk_locn[%d].offset %llu", di,
			  (unsigned long long)xlate64(dlocn->offset));
		log_print("pv_header.disk_locn[%d].size %llu", di,
			  (unsigned long long)xlate64(dlocn->size));
	}

	/* advance past the all-zero dlocn struct */
	di++;
	dlocn++;
	dlocn_offset += 16;

	/*
	 * pv_header_extension follows the last disk_locn
	 * terminating struct, so it's not always at the
	 * same location.
	 */

	pvhe = (struct pv_header_extension *)dlocn;
	pvhe_offset = dlocn_offset; /* from start of disk */

	/* sanity check */
	if ((void *)pvhe != (void *)(buf + pvhe_offset - lh_offset))
		log_print("CHECK: problem with pv_header_extension offset calculation");

	if (print_fields) {
		log_print("pv_header_extension at %llu", (unsigned long long)pvhe_offset);
		log_print("pv_header_extension.version %u", xlate32(pvhe->version));
		log_print("pv_header_extension.flags %u", xlate32(pvhe->flags));
	}

	/*
	 * The pv_header_extension is 8 bytes, excluding disk_locn's.
	 * disk_locn structs immediately follow the pv_header_extension.
	 * Each disk_locn is 16 bytes.
	 */
	di = 0;
	dlocn = pvhe->bootloader_areas_xl;
	dlocn_offset = pvhe_offset + 8;

	while ((tmp = xlate64(dlocn->offset))) {
		if (print_fields) {
			log_print("pv_header_extension.disk_locn[%d] at %llu # bootloader area", di,
				  (unsigned long long)dlocn_offset);
			log_print("pv_header_extension.disk_locn[%d].offset %llu", di,
				  (unsigned long long)xlate64(dlocn->offset));
			log_print("pv_header_extension.disk_locn[%d].size %llu", di,
				  (unsigned long long)xlate64(dlocn->size));
		}

		di++;
		dlocn++;
		dlocn_offset += 16;
	}

	/* all-zero dlocn struct is area list end */
	if (print_fields) {
		log_print("pv_header_extension.disk_locn[%d] at %llu # location list end", di,
			  (unsigned long long) dlocn_offset);
		log_print("pv_header_extension.disk_locn[%d].offset %llu", di,
			  (unsigned long long)xlate64(dlocn->offset));
		log_print("pv_header_extension.disk_locn[%d].size %llu", di,
			  (unsigned long long)xlate64(dlocn->size));
	}

	if (bad)
		return 0;
	return 1;
}

/*
 * mda_offset and mda_size are the location/size of the metadata area,
 * which starts with the mda_header and continues through the circular
 * buffer of text.
 *
 * mda_offset and mda_size values come from the pv_header/disk_locn,
 * which could be incorrect.
 *
 * We know that the first mda_offset will always be 4096, so we use
 * that value regardless of what the first mda_offset value in the
 * pv_header is.
 */

static int _dump_mda_header(struct cmd_context *cmd,
			    int print_fields, int print_metadata, int print_area,
			    const char *tofile,
			    struct device *dev,
			    uint64_t mda_offset, uint64_t mda_size,
			    uint32_t *checksum0_ret)
{
	char str[256];
	char *buf;
	struct mda_header *mh;
	struct raw_locn *rlocn0, *rlocn1;
	uint64_t rlocn0_offset, rlocn1_offset;
	uint64_t meta_offset = 0;
	uint64_t meta_size = 0;
	uint32_t meta_checksum = 0;
	int mda_num = (mda_offset == 4096) ? 1 : 2;
	int bad = 0;

	*checksum0_ret = 0; /* checksum from raw_locn[0] */

	if (!(buf = zalloc(512)))
		return_0;

	/*
	 * The first mda_header is 4096 bytes from the start
	 * of the device.  Each mda_header is 512 bytes.
	 *
	 * The start/size values in the mda_header should
	 * match the mda_offset/mda_size values that came
	 * from the pv_header/disk_locn.
	 *
	 * (Why was mda_header magic made only partially printable?)
	 */

	if (!dev_read_bytes(dev, mda_offset, 512, buf)) {
		log_print("CHECK: failed to read mda_header at %llu", (unsigned long long)mda_offset);
		return 0;
	}

	mh = (struct mda_header *)buf;

	if (print_fields) {
		log_print("mda_header_%d at %llu # metadata area", mda_num, (unsigned long long)mda_offset);
		log_print("mda_header_%d.checksum 0x%x", mda_num, xlate32(mh->checksum_xl));
		log_print("mda_header_%d.magic 0x%s", mda_num, _chars_to_hexstr(mh->magic, str, 16, 256, "mda_header.magic"));
		log_print("mda_header_%d.version %u", mda_num, xlate32(mh->version));
		log_print("mda_header_%d.start %llu", mda_num, (unsigned long long)xlate64(mh->start));
		log_print("mda_header_%d.size %llu", mda_num, (unsigned long long)xlate64(mh->size));
	}

	if (!_check_mda_header(mh, mda_num, mda_offset, mda_size))
		bad++;

	if (print_area) {
		if (!_dump_meta_area(dev, tofile, mda_offset, mda_size))
			bad++;
		goto out;
	}

	/*
	 * mda_header is 40 bytes, the raw_locn structs
	 * follow immediately after, each raw_locn struct
	 * is 24 bytes.
	 */

	rlocn0 = mh->raw_locns;
	rlocn0_offset = mda_offset + 40; /* from start of disk */

	/* sanity check */
	if ((void *)rlocn0 != (void *)(buf + rlocn0_offset - mda_offset))
		log_print("CHECK: problem with rlocn0 offset calculation");

	meta_offset = 0;
	meta_size = 0;
	meta_checksum = 0;

	if (!_dump_raw_locn(dev, print_fields, rlocn0, 0, rlocn0_offset, mda_num, mda_offset, mda_size,
			    &meta_offset, &meta_size, &meta_checksum))
		bad++;

	*checksum0_ret = meta_checksum;

	rlocn1 = (struct raw_locn *)((char *)mh->raw_locns + 24);
	rlocn1_offset = rlocn0_offset + 24;

	/* sanity check */
	if ((void *)rlocn1 != (void *)(buf + rlocn1_offset - mda_offset))
		log_print("CHECK: problem with rlocn1 offset calculation");

	if (!_dump_raw_locn(dev, print_fields, rlocn1, 1, rlocn1_offset, mda_num, mda_offset, mda_size,
			   NULL, NULL, NULL))
		bad++;

	if (!meta_offset)
		goto out;

	if (!_dump_meta_text(dev, print_fields, print_metadata, tofile, mda_num, 0, mda_offset, mda_size, meta_offset, meta_size, meta_checksum))
		bad++;

	/* Should we also check text metadata if it exists in rlocn1? */
 out:
	if (bad)
		return 0;
	return 1;
}

static int _dump_headers(struct cmd_context *cmd,
			 int argc, char **argv)
{
	struct device *dev;
	const char *pv_name;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0;
	uint32_t mda1_checksum, mda2_checksum;
	int bad = 0;

	pv_name = argv[0];

	if (!(dev = dev_cache_get(cmd, pv_name, cmd->filter))) {
		log_error("No device found for %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
		return ECMD_FAILED;
	}

	label_scan_setup_bcache();

	if (!_dump_label_and_pv_header(cmd, 1, dev,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size))
		bad++;

	/* N.B. mda1_size and mda2_size may be different */

	/*
	 * The first mda is always 4096 bytes from the start of the device.
	 *
	 * TODO: A second mda may not exist.  If the pv_header says there
	 * is no second mda, we may still want to check for a second mda
	 * in case it's the pv_header that is wrong.  Try looking for
	 * an mda_header at 1MB prior to the end of the device, if
	 * mda2_offset is 0 or if we don't find an mda_header at mda2_offset
	 * which may have been corrupted.
	 */

	if (!_dump_mda_header(cmd, 1, 0, 0, NULL, dev, 4096, mda1_size, &mda1_checksum))
		bad++;

	/*
	 * mda2_offset may be incorrect.  Probe for a valid mda_header at
	 * mda2_offset and at other possible/expected locations, e.g.
	 * 1MB before end of device.  Call dump_mda_header with a different
	 * offset than mda2_offset if there's no valid header at mda2_offset 
	 * but there is a valid header elsewhere.
	 */
	if (mda2_offset) {
		if (!_dump_mda_header(cmd, 1, 0, 0, NULL, dev, mda2_offset, mda2_size, &mda2_checksum))
			bad++;

		/* This probably indicates that one was committed and the other not. */
		if (mda1_checksum && mda2_checksum && (mda1_checksum != mda2_checksum))
			log_print("CHECK: mdas have different raw_locn[0].checksum values");
	}

	if (bad) {
		log_error("Found bad header or metadata values.");
		return ECMD_FAILED;
	}
	return ECMD_PROCESSED;
}

static int _dump_metadata(struct cmd_context *cmd,
			 int argc, char **argv, int full_area)
{
	struct device *dev;
	const char *pv_name;
	const char *tofile = NULL;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0;
	uint32_t mda1_checksum, mda2_checksum;
	int print_metadata = !full_area;
	int print_area = full_area;
	int mda_num = 1;
	int bad = 0;

	if (arg_is_set(cmd, file_ARG)) {
		if (!(tofile = arg_str_value(cmd, file_ARG, NULL)))
			return ECMD_FAILED;
	}

	/* 1: dump metadata from first mda, 2: dump metadata from second mda */
	if (arg_is_set(cmd, pvmetadatacopies_ARG))
		mda_num = arg_int_value(cmd, pvmetadatacopies_ARG, 1);

	pv_name = argv[0];

	if (!(dev = dev_cache_get(cmd, pv_name, cmd->filter))) {
		log_error("No device found for %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
		return ECMD_FAILED;
	}

	label_scan_setup_bcache();

	if (!_dump_label_and_pv_header(cmd, 0, dev,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size))
		bad++;


	/*
	 * The first mda is always 4096 bytes from the start of the device.
	 *
	 * TODO: A second mda may not exist.  If the pv_header says there
	 * is no second mda, we may still want to check for a second mda
	 * in case it's the pv_header that is wrong.  Try looking for
	 * an mda_header at 1MB prior to the end of the device, if
	 * mda2_offset is 0 or if we don't find an mda_header at mda2_offset
	 * which may have been corrupted.
	 *
	 * mda2_offset may be incorrect.  Probe for a valid mda_header at
	 * mda2_offset and at other possible/expected locations, e.g.
	 * 1MB before end of device.  Call dump_mda_header with a different
	 * offset than mda2_offset if there's no valid header at mda2_offset 
	 * but there is a valid header elsewhere.
	 */

	if (mda_num == 1) {
		if (!_dump_mda_header(cmd, 0, print_metadata, print_area, tofile, dev, 4096, mda1_size, &mda1_checksum))
			bad++;
	} else if (mda_num == 2) {
		if (!mda2_offset) {
			log_print("CHECK: second mda not found");
			bad++;
		} else {
			if (!_dump_mda_header(cmd, 0, print_metadata, print_area, tofile, dev, mda2_offset, mda2_size, &mda2_checksum))
				bad++;
		}
	}

	if (bad) {
		log_error("Found bad header or metadata values.");
		return ECMD_FAILED;
	}
	return ECMD_PROCESSED;
}

int pvck(struct cmd_context *cmd, int argc, char **argv)
{
	struct dm_list devs;
	struct device_list *devl;
	struct device *dev;
	const char *dump;
	const char *pv_name;
	uint64_t labelsector;
	int i;
	int ret_max = ECMD_PROCESSED;

	if (arg_is_set(cmd, dump_ARG)) {
		dump = arg_str_value(cmd, dump_ARG, NULL);

		if (!strcmp(dump, "metadata"))
			return _dump_metadata(cmd, argc, argv, 0);

		if (!strcmp(dump, "metadata_area"))
			return _dump_metadata(cmd, argc, argv, 1);

		if (!strcmp(dump, "headers"))
			return _dump_headers(cmd, argc, argv);

		log_error("Unknown dump value.");
		return ECMD_FAILED;
	}

	labelsector = arg_uint64_value(cmd, labelsector_ARG, UINT64_C(0));

	dm_list_init(&devs);

	for (i = 0; i < argc; i++) {
		dm_unescape_colons_and_at_signs(argv[i], NULL, NULL);

		pv_name = argv[i];

		dev = dev_cache_get(cmd, pv_name, cmd->filter);

		if (!dev) {
			log_error("Device %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
			continue;
		}

		if (!(devl = zalloc(sizeof(*devl))))
			continue;

		devl->dev = dev;
		dm_list_add(&devs, &devl->list);
	}

	label_scan_setup_bcache();
	label_scan_devs(cmd, cmd->filter, &devs);

	dm_list_iterate_items(devl, &devs) {
		/*
		 * The scan above will populate lvmcache with any info from the
		 * standard locations at the start of the device.  Now populate
		 * lvmcache with any info from non-standard offsets.
		 *
		 * FIXME: is it possible for a real lvm label sector to be
		 * anywhere other than the first four sectors of the disk?
		 * If not, drop the code in label_read_sector/find_lvm_header
		 * that supports searching at any sector.
		 */
		if (labelsector) {
			if (!label_read_sector(devl->dev, labelsector)) {
				stack;
				ret_max = ECMD_FAILED;
				continue;
			}
		}

		if (!pv_analyze(cmd, devl->dev, labelsector)) {
			stack;
			ret_max = ECMD_FAILED;
		}
	}

	return ret_max;
}
