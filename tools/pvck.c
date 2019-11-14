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

#define PRINT_CURRENT 1
#define PRINT_ALL 2

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

static int _check_vgname_start(char *buf, int *len)
{
	int chars = 0;
	int space = 0;
	int i;
	char c;

	/*
	 * Valid metadata begins: 'vgname {'
	 */
	for (i = 0; i <= NAME_LEN + 2; i++) {
		c = buf[i];

		if (isalnum(c) || c == '.' || c == '_' || c == '-' || c == '+') {
			if (space)
				return 0;
			chars++;
			continue;
		}

		if (c == ' ') {
			if (!chars || space)
				return 0;
			space++;
			continue;
		}

		if (c == '{') {
			if (chars && space) {
				*len = chars;
				return 1;
			}
			return 0;
		}

		return 0;
	}
	return 0;
}

static void _copy_out_metadata(char *buf, uint32_t start, uint32_t first_start, uint64_t mda_size, char **meta_buf, uint64_t *meta_size, int *bad_end)
{
	char *new_buf;
	uint64_t i;
	uint64_t new_len;
	uint64_t len_a = 0, len_b = 0;
	uint32_t stop;
	int found_end;

	/*
	 * If we wrap around the buffer searching for the
	 * end of some metadata, either stop when we reach
	 * where we began (start), or stop where we found
	 * the first copy of metadata (first_start).
	 */
	if (!first_start)
		stop = start;
	else
		stop = first_start;

	found_end = 0;
	for (i = start; i < mda_size; i++) {
		if (buf[i] == '\0') {
			found_end = 1;
			break;
		}
	}

	if (found_end) {
		new_len = i - start;
	} else {
		len_a = i - start;

		found_end = 0;
		for (i = 512; i < stop; i++) {
			if (buf[i] == '\0') {
				found_end = 1;
				break;
			}
		}

		if (!found_end)
			return;

		len_b = i - 512;
		new_len = len_a + len_b;
	}

	if (new_len < 256) {
		log_print("skip invalid metadata with len %llu at %llu",
			  (unsigned long long)new_len, (unsigned long long)start);
		return;
	}

	/* terminating 0 byte */
	new_len++;

	if (!(new_buf = malloc(new_len)))
		return;

	memset(new_buf, 0, new_len);

	if (len_a) {
		memcpy(new_buf, buf+start, len_a);
		memcpy(new_buf+len_a, buf+512, len_b);
	} else {
		memcpy(new_buf, buf+start, new_len);
	}

	/* \0 should be preceded by \n\n (0x0a0a) */
	if (new_buf[new_len-1] != 0 || new_buf[new_len-2] != 0x0a || new_buf[new_len-3] != 0x0a)
		*bad_end = 1;

	*meta_buf = new_buf;
	*meta_size = new_len;
}

static int _text_buf_parsable(char *text_buf, uint64_t text_size)
{
	struct dm_config_tree *cft;

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, NULL, 0))) {
		return 0;
	}

	if (!dm_config_parse(cft, text_buf, text_buf + text_size)) {
		config_destroy(cft);
		return 0;
	}

	config_destroy(cft);
	return 1;
}

#define MAX_LINE_CHECK 128
#define ID_STR_SIZE 48

static void _copy_line(char *in, char *out, int *len)
{
	int i;

	*len = 0;

	for (i = 0; i < MAX_LINE_CHECK; i++) {
		if ((in[i] == '\n') || (in[i] == '\0'))
			break;
		out[i] = in[i];
	}
	*len = i+1;
}

static int _dump_all_text(struct cmd_context *cmd, const char *tofile, struct device *dev,
			  int mda_num, uint64_t mda_offset, uint64_t mda_size, char *buf)
{
	FILE *fp = NULL;
	char line[MAX_LINE_CHECK];
	char vgname[NAME_LEN+1];
	char id_str[ID_STR_SIZE];
	char id_first[ID_STR_SIZE];
	char *text_buf;
	char *p;
	uint32_t buf_off; /* offset with buf which begins with mda_header */
	uint32_t buf_off_first = 0;
	uint32_t seqno;
	uint32_t crc;
	uint64_t text_size;
	uint64_t meta_size;
	int multiple_vgs = 0;
	int bad_end;
	int vgnamelen;
	int count;
	int len;

	if (tofile) {
		if (!(fp = fopen(tofile, "wx"))) {
			log_error("Failed to create file %s", tofile);
			return 0;
		}
	}

	/*
	 * If metadata has not wrapped, and the metadata area beginning
	 * has not been damaged, the text area will begin with vgname {.
	 * Wrapping or damage would mean we find no metadata starting at
	 * the start of the area.
	 *
	 * Try looking at each 512 byte offset within the area for the start
	 * of another copy of metadata.  Metadata copies have begun at 512
	 * aligned offsets since very early lvm2 code in 2002.
	 *
	 * (We could also search for something definitive like
	 * "# Generated by LVM2" in the area, and then work backward to find
	 * a likely beginning.)
	 *
	 * N.B. This relies on VG metadata first having the id = "..." field
	 * followed by the "seqno = N" field.
	 */

	memset(id_first, 0, sizeof(id_str));

	/*
	 * A count of 512 byte chunks within the metadata area.
	 */
	count = 0;

	meta_size = mda_size - 512;

	/*
	 * Search 512 byte boundaries for the start of new metadata copies.
	 */
	while (count < (meta_size / 512)) {
		memset(vgname, 0, sizeof(vgname));
		memset(id_str, 0, sizeof(id_str));
		seqno = 0;
		vgnamelen = 0;
		text_size = 0;
		bad_end = 0;

		/*
		 * Check for a new metadata copy at each 512 offset
		 * (after skipping 512 bytes for mda_header at the
		 * start of the buf).
		 *
		 * If a line looks like it begins with a vgname
		 * it could be a new copy of metadata, but it could
		 * also be a random bit of metadata that looks like
		 * a vgname, so confirm it's the start of metadata
		 * by looking for id and seqno lines following the
		 * possible vgname.
		 */
		buf_off = 512 + (count * 512);
		p = buf + buf_off;

		/*
		 * copy line of possible metadata to check for vgname
		 */
		memset(line, 0, sizeof(line));
		_copy_line(p, line, &len);
		p += len;

		if (!_check_vgname_start(line, &vgnamelen)) {
			count++;
			continue;
		}

		memcpy(vgname, line, vgnamelen);

		/*
		 * copy next line of metadata, which should contain id
		 */
		memset(line, 0, sizeof(line));
		_copy_line(p, line, &len);
		p += len;

		if (strncmp(line, "id = ", 5)) {
			count++;
			continue;
		}

		memcpy(id_str, line + 6, 38);

		/*
		 * copy next line of metadata, which should contain seqno
		 */
		memset(line, 0, sizeof(line));
		_copy_line(p, line, &len);
		p += len;

		if (strncmp(line, "seqno = ", 8)) {
			count++;
			continue;
		}

		sscanf(line, "seqno = %u", &seqno);

		/*
		 * The first three lines look like metadata with
		 * vgname/id/seqno, so copy out the full metadata.
		 *
		 * If this reaches the end of buf without reaching the
		 * end marker of metadata, it will wrap around to the
		 * start of buf and continue copying until it reaches
		 * a NL or until it reaches buf_off_first (which is
		 * where we've already taken text from.)
		 */
		_copy_out_metadata(buf, buf_off, buf_off_first, mda_size, &text_buf, &text_size, &bad_end);

		if (!text_buf) {
			log_warn("Failed to extract full metadata text at %llu, skipping.",
				 (unsigned long long)(mda_offset + buf_off));
			count++;
			continue;
		}

		/*
		 * check if it's finding metadata from different vgs
		 */
		if (!id_first[0])
			memcpy(id_first, id_str, sizeof(id_first));
		else if (memcmp(id_first, id_str, sizeof(id_first)))
			multiple_vgs = 1;

		crc = calc_crc(INITIAL_CRC, (uint8_t *)text_buf, text_size);

		log_print("metadata at %llu length %llu crc %08x vg %s seqno %u id %s",
			  (unsigned long long)(mda_offset + buf_off),
			  (unsigned long long)text_size,
			  crc, vgname, seqno, id_str);

		/*
		 * save the location of the first metadata we've found so
		 * we know where to stop after wrapping buf.
		 */
		if (!buf_off_first)
			buf_off_first = buf_off;

		/*
		 * check if the full metadata is parsable
		 */

		if (!_text_buf_parsable(text_buf, text_size))
			log_warn("WARNING: parse error for metadata at %llu", (unsigned long long)(mda_offset + buf_off));
		if (bad_end)
			log_warn("WARNING: bad terminating bytes for metadata at %llu", (unsigned long long)(mda_offset + buf_off));

		if (arg_is_set(cmd, verbose_ARG)) {
			char *str1, *str2;
			if ((str1 = strstr(text_buf, "description = "))) {
				memset(line, 0, sizeof(line));
				_copy_line(str1, line, &len);
				log_print("%s", line);
				if ((str2 = strstr(str1, "creation_time = "))) {
					memset(line, 0, sizeof(line));
					_copy_line(str2, line, &len);
					log_print("%s\n", line);
				}
			}
		}

		if (fp) {
			fprintf(fp, "%s", text_buf);
			fprintf(fp, "\n--\n");
		}

		free(text_buf);
		text_buf = NULL;

		if (text_size < 512)
			count++;
		else if (!(text_size % 512))
			count += (text_size / 512);
		else
			count += ((text_size / 512) + 1);
	}

	if (multiple_vgs)
		log_warn("WARNING: metadata from multiple VGs was found.");

	if (fp) {
		if (fflush(fp))
			stack;
		if (fclose(fp))
			stack;
	}

	return 1;
}

static int _check_label_header(struct label_header *lh, uint64_t labelsector,
			       int *found_label)
{
	uint32_t crc;
	int good_id = 1, good_type = 1;
	int bad = 0;

	if (memcmp(lh->id, LABEL_ID, sizeof(lh->id))) {
		log_print("CHECK: label_header.id expected %s", LABEL_ID);
		good_id = 0;
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
		good_type = 0;
		bad++;
	}

	/* Report a label is found if at least id and type are correct. */
	if (found_label && good_id && good_type)
		*found_label = 1;

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
static int _check_mda_header(struct mda_header *mh, int mda_num, uint64_t mda_offset, uint64_t mda_size, int *found_header)
{
	char str[256];
	uint32_t crc;
	int good_magic = 1;
	int bad = 0;

	crc = calc_crc(INITIAL_CRC, (uint8_t *)mh->magic,
		       MDA_HEADER_SIZE - sizeof(mh->checksum_xl));

	if (crc != xlate32(mh->checksum_xl)) {
		log_print("CHECK: mda_header_%d.checksum expected 0x%x", mda_num, crc);
		bad++;
	}

	if (memcmp(mh->magic, FMTT_MAGIC, sizeof(mh->magic))) {
		log_print("CHECK: mda_header_%d.magic expected 0x%s", mda_num, _chars_to_hexstr((void *)&FMTT_MAGIC, str, 16, 256, "mda_header.magic"));
		good_magic = 0;
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

	/* Report a header is found if at least magic is correct. */
	if (found_header && good_magic)
		*found_header = 1;

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

	if (!(meta_buf = zalloc(mda_size)))
		return_0;

	if (!dev_read_bytes(dev, mda_offset, mda_size, meta_buf)) {
		log_print("CHECK: failed to read metadata area at offset %llu size %llu",
			  (unsigned long long)mda_offset, (unsigned long long)mda_size);
		free(meta_buf);
		return 0;
	}

	if (!(fp = fopen(tofile, "wx"))) {
		log_error("Failed to create file %s.", tofile);
		free(meta_buf);
		return 0;
	}

	if (fwrite(meta_buf, mda_size - 512, 1, fp) < (mda_size - 512))
		log_warn("WARNING: Failed to write " FMTu64 " bytes to file %s.", mda_size - 512, tofile);

	free(meta_buf);
	if (fflush(fp))
		stack;
	if (fclose(fp))
		stack;

	return 1;
}

/*
 * Search for any instance of id_str[] in the metadata area,
 * where the id_str indicates the start of a metadata copy
 * (which could be complete or a fragment.)
 * id_str is an open brace followed by id = <uuid>.
 *
 * {\n
 *    id = "lL7Mnk-oCGn-Bde2-9B6S-44Z7-VrHa-wvfC3v"
 *
 * 1\23456789012345678901234567890123456789012345678
 *          10        20        30        40
 */

static int _dump_current_text(struct device *dev,
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
	uint32_t seqno = 0;
	int mn = mda_num; /* 1 or 2 */
	int ri = rlocn_index; /* 0 or 1 */
	int bad = 0;

	if (!(meta_buf = zalloc(meta_size))) {
		log_print("CHECK: mda_header_%d.raw_locn[%d] no mem for metadata text size %llu", mn, ri,
			  (unsigned long long)meta_size);
		return 0;
	}

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
			free(meta_buf);
			return 0;
		}

		if (!dev_read_bytes(dev, offset_b, size_b, meta_buf + size_a)) {
			log_print("CHECK: failed to read metadata text at mda_header_%d.raw_locn[%d].offset %llu size %llu part_b %llu %llu", mn, ri,
				  (unsigned long long)meta_offset, (unsigned long long)meta_size,
				  (unsigned long long)offset_b, (unsigned long long)size_b);
			free(meta_buf);
			return 0;
		}
	} else {
		if (!dev_read_bytes(dev, mda_offset + meta_offset, meta_size, meta_buf)) {
			log_print("CHECK: failed to read metadata text at mda_header_%d.raw_locn[%d].offset %llu size %llu", mn, ri,
				  (unsigned long long)meta_offset, (unsigned long long)meta_size);
			free(meta_buf);
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
			if (cft->root && cft->root->key)
				vgname = strdup(cft->root->key);
			if (cft->root && cft->root->child)
				dm_config_get_uint32(cft->root->child, "seqno", &seqno);
		}
		config_destroy(cft);
	}

	if (print_fields || print_metadata)
		log_print("metadata text at %llu crc 0x%x # vgname %s seqno %u",
			  (unsigned long long)(mda_offset + meta_offset), crc,
			  vgname ? vgname : "?", seqno);

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
	free(meta_buf);

	return (!bad) ? 1 : 0;
}

static int _dump_label_and_pv_header(struct cmd_context *cmd, int print_fields,
				     struct device *dev,
				     int *found_label,
				     uint64_t *mda1_offset, uint64_t *mda1_size,
				     uint64_t *mda2_offset, uint64_t *mda2_size,
				     int *mda_count_out)
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
		free(buf);
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

	if (!_check_label_header(lh, labelsector, found_label))
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

	*mda_count_out = mda_count;

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

	free(buf);

	return (!bad) ? 1 : 0;
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
			    uint32_t *checksum0_ret,
			    int *found_header)
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
		free(buf);
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

	if (!_check_mda_header(mh, mda_num, mda_offset, mda_size, found_header))
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

	/*
	 * looking at the current copy of metadata referenced by raw_locn
	 */
	if (print_metadata <= PRINT_CURRENT) {
		if (!_dump_current_text(dev, print_fields, print_metadata, tofile, mda_num, 0, mda_offset, mda_size, meta_offset, meta_size, meta_checksum))
			bad++;
	}

	/*
	 * looking at all copies of the metadata in the area
	 */
	if (print_metadata == PRINT_ALL) {
		free(buf);

		if (!(buf = malloc(mda_size)))
			goto_out;
		memset(buf, 0, mda_size);

		if (!dev_read_bytes(dev, mda_offset, mda_size, buf)) {
			log_print("CHECK: failed to read metadata area at offset %llu size %llu",
				  (unsigned long long)mda_offset, (unsigned long long)mda_size);
			bad++;
			goto out;
		}

		_dump_all_text(cmd, tofile, dev, mda_num, mda_offset, mda_size, buf);
	}

	/* Should we also check text metadata if it exists in rlocn1? */
 out:
	free(buf);

	return (!bad) ? 1 : 0;
}

static int _dump_headers(struct cmd_context *cmd,
			 int argc, char **argv)
{
	struct device *dev;
	const char *pv_name;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0;
	uint32_t mda1_checksum, mda2_checksum;
	int mda_count = 0;
	int bad = 0;

	pv_name = argv[0];

	if (!(dev = dev_cache_get(cmd, pv_name, cmd->filter))) {
		log_error("No device found for %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
		return ECMD_FAILED;
	}

	label_scan_setup_bcache();

	if (!_dump_label_and_pv_header(cmd, 1, dev, NULL,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count))
		bad++;

	if (!mda_count) {
		log_print("zero metadata copies");
		return ECMD_PROCESSED;
	}

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

	if (!_dump_mda_header(cmd, 1, 0, 0, NULL, dev, 4096, mda1_size, &mda1_checksum, NULL))
		bad++;

	/*
	 * mda2_offset may be incorrect.  Probe for a valid mda_header at
	 * mda2_offset and at other possible/expected locations, e.g.
	 * 1MB before end of device.  Call dump_mda_header with a different
	 * offset than mda2_offset if there's no valid header at mda2_offset 
	 * but there is a valid header elsewhere.
	 */
	if (mda2_offset) {
		if (!_dump_mda_header(cmd, 1, 0, 0, NULL, dev, mda2_offset, mda2_size, &mda2_checksum, NULL))
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
			 int argc, char **argv,
			 int print_metadata, int print_area)
{
	struct device *dev;
	const char *pv_name;
	const char *tofile = NULL;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0;
	uint32_t mda1_checksum, mda2_checksum;
	int mda_count = 0;
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

	if (!_dump_label_and_pv_header(cmd, 0, dev, NULL,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count))
		bad++;

	if (!mda_count) {
		log_print("zero metadata copies");
		return ECMD_PROCESSED;
	}

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
		if (!_dump_mda_header(cmd, 0, print_metadata, print_area, tofile, dev, 4096, mda1_size, &mda1_checksum, NULL))
			bad++;
	} else if (mda_num == 2) {
		if (!mda2_offset) {
			log_print("CHECK: second mda not found");
			bad++;
		} else {
			if (!_dump_mda_header(cmd, 0, print_metadata, print_area, tofile, dev, mda2_offset, mda2_size, &mda2_checksum, NULL))
				bad++;
		}
	}

	if (bad) {
		log_error("Found bad header or metadata values.");
		return ECMD_FAILED;
	}
	return ECMD_PROCESSED;
}

static int _dump_found(struct cmd_context *cmd, struct device *dev,
		       uint64_t labelsector)
{
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0;
	uint32_t mda1_checksum = 0, mda2_checksum = 0;
	int found_label = 0, found_header1 = 0, found_header2 = 0;
	int mda_count = 0;
	int bad = 0;

	if (!_dump_label_and_pv_header(cmd, 0, dev, &found_label,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count))
		bad++;

	if (found_label && mda1_offset) {
		if (!_dump_mda_header(cmd, 0, 0, 0, NULL, dev, 4096, mda1_size, &mda1_checksum, &found_header1))
			bad++;
	}

	if (found_label && mda2_offset) {
		if (!_dump_mda_header(cmd, 0, 0, 0, NULL, dev, mda2_offset, mda2_size, &mda2_checksum, &found_header2))
			bad++;
	}

	if (found_label)
		log_print("Found label on %s, sector %llu, type=LVM2 001",
			  dev_name(dev), (unsigned long long)labelsector);
	else {
		log_error("Could not find LVM label on %s", dev_name(dev));
		return 0;
	}

	if (found_header1)
		log_print("Found text metadata area: offset=%llu, size=%llu",
			  (unsigned long long)mda1_offset,
			  (unsigned long long)mda1_size);

	if (found_header2)
		log_print("Found text metadata area: offset=%llu, size=%llu",
			  (unsigned long long)mda2_offset,
			  (unsigned long long)mda2_size);

	if (bad)
		return 0;
	return 1;
}

#define ONE_MB_IN_BYTES 1048576

/*
 * Look for metadata text in common locations, without using any headers
 * (pv_header/mda_header) to find the location, since the headers may be
 * zeroed/damaged.
 */

static int _dump_search(struct cmd_context *cmd,
			int argc, char **argv)
{
	char str[256];
	struct device *dev;
	const char *pv_name;
	const char *tofile = NULL;
	char *buf;
	struct mda_header *mh;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0;
	uint64_t mda_offset, mda_size;
	int found_header = 0;
	int mda_count = 0;
	int mda_num = 1;

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

	_dump_label_and_pv_header(cmd, 0, dev, NULL,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count);

	/*
	 * TODO: allow mda_offset and mda_size to be specified on the
	 * command line.
	 *
	 * For mda1, mda_offset is always 4096 bytes from the start of
	 * device, and mda_size is the space between mda_offset and
	 * the first PE which is usually at 1MB.
	 *
	 * For mda2, take the dev_size, reduce that to be a 1MB
	 * multiple.  The mda_offset is then 1MB prior to that,
	 * and mda_size is the amount of space between that offset
	 * and the end of the device.
	 *
	 * The second mda is generally 4K larger (at least) than the
	 * first mda because the first mda begins at a 4K offset from
	 * the start of the device and ends on a 1MB boundary.
	 * The second mda begins on a 1MB boundary (no 4K offset like
	 * mda1), then goes to the end of the device.  Extra space
	 * at the end of device (mod 1MB extra) can make mda2 even
	 * larger.
	 */
	if (mda_num == 1) {
		mda_offset = 4096;
		mda_size = ONE_MB_IN_BYTES - 4096;
	} else if (mda_num == 2) {
		uint64_t dev_sectors = 0;
		uint64_t dev_bytes;
		uint64_t extra_bytes;

		if (!dev_get_size(dev, &dev_sectors))
			return_ECMD_FAILED;

		dev_bytes = dev_sectors * 512;
		extra_bytes = dev_bytes % ONE_MB_IN_BYTES;

		if (dev_bytes < (2 * ONE_MB_IN_BYTES))
			return ECMD_FAILED;

		mda_offset = dev_bytes - extra_bytes - ONE_MB_IN_BYTES;
		mda_size = dev_bytes - mda_offset;
	}

	if ((mda_num == 1) && (mda1_offset != mda_offset)) {
		log_print("Ignoring mda1_offset %llu mda1_size %llu from pv_header.",
			  (unsigned long long)mda1_offset,
			  (unsigned long long)mda1_size);
	}

	if ((mda_num == 2) && (mda2_offset != mda_offset)) {
		log_print("Ignoring mda2_size %llu mda2_offset %llu from pv_header.",
			  (unsigned long long)mda2_offset,
			  (unsigned long long)mda2_size);
	}

	log_print("Searching for metadata in mda%d at offset %llu size %llu", mda_num,
		  (unsigned long long)mda_offset, (unsigned long long)mda_size);

	if (!(buf = zalloc(mda_size)))
		return ECMD_FAILED;

	if (!dev_read_bytes(dev, mda_offset, mda_size, buf)) {
		log_print("CHECK: failed to read metadata area at offset %llu size %llu",
			   (unsigned long long)mda_offset, (unsigned long long)mda_size);
		free(buf);
		return ECMD_FAILED;
	}

	mh = (struct mda_header *)buf;

	/* Can be useful to know if there's a valid mda_header at this location. */
	log_print("mda_header_%d at %llu # metadata area", mda_num, (unsigned long long)mda_offset);
	log_print("mda_header_%d.checksum 0x%x", mda_num, xlate32(mh->checksum_xl));
	log_print("mda_header_%d.magic 0x%s", mda_num, _chars_to_hexstr(mh->magic, str, 16, 256, "mda_header.magic"));
	log_print("mda_header_%d.version %u", mda_num, xlate32(mh->version));
	log_print("mda_header_%d.start %llu", mda_num, (unsigned long long)xlate64(mh->start));
	log_print("mda_header_%d.size %llu", mda_num, (unsigned long long)xlate64(mh->size));

	_check_mda_header(mh, mda_num, mda_offset, mda_size, &found_header);

	log_print("searching for metadata text");

	_dump_all_text(cmd, tofile, dev, mda_num, mda_offset, mda_size, buf);

	free(buf);
	return ECMD_PROCESSED;
}

int pvck(struct cmd_context *cmd, int argc, char **argv)
{
	struct device *dev;
	const char *dump;
	const char *pv_name;
	uint64_t labelsector = 1;
	int bad = 0;
	int i;

	if (arg_is_set(cmd, dump_ARG)) {
		dump = arg_str_value(cmd, dump_ARG, NULL);

		if (!strcmp(dump, "metadata"))
			return _dump_metadata(cmd, argc, argv, PRINT_CURRENT, 0);

		if (!strcmp(dump, "metadata_all"))
			return _dump_metadata(cmd, argc, argv, PRINT_ALL, 0);

		if (!strcmp(dump, "metadata_area"))
			return _dump_metadata(cmd, argc, argv, 0, 1);

		if (!strcmp(dump, "metadata_search"))
			return _dump_search(cmd, argc, argv);

		if (!strcmp(dump, "headers"))
			return _dump_headers(cmd, argc, argv);

		log_error("Unknown dump value.");
		return ECMD_FAILED;
	}

	/*
	 * The old/original form of pvck, which did not do much,
	 * but this is here to preserve the historical output.
	 */

	if (arg_is_set(cmd, labelsector_ARG))
		labelsector = arg_uint64_value(cmd, labelsector_ARG, UINT64_C(0));

	label_scan_setup_bcache();

	for (i = 0; i < argc; i++) {
		pv_name = argv[i];

		if (!(dev = dev_cache_get(cmd, argv[i], cmd->filter))) {
			log_error("Device %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
			continue;
		}

		if (!_dump_found(cmd, dev, labelsector))
			bad++;
	}

	if (bad)
		return ECMD_FAILED;
	return ECMD_PROCESSED;
}
