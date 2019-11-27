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

#define ID_STR_SIZE 40 /* uuid formatted with dashes is 38 chars */

/*
 * command line input from --settings
 */
struct settings {
	uint64_t metadata_offset;  /* bytes, start of text metadata (from start of disk) */
	uint64_t mda_offset;       /* bytes, start of mda_header (from start of disk) */
	uint64_t mda_size;         /* bytes, size of metadata area (mda_header + text area) */
	uint64_t mda2_offset;      /* bytes */
	uint64_t mda2_size;        /* bytes */
	uint64_t device_size;      /* bytes */
	uint64_t data_offset;      /* bytes, start of data (pe_start) */
	uint32_t seqno;
	struct id pvid;

	int mda_num;               /* 1 or 2 for first or second mda */
	char *backup_file;

	unsigned metadata_offset_set:1;
	unsigned mda_offset_set:1;
	unsigned mda_size_set:1;
	unsigned mda2_offset_set;
	unsigned mda2_size_set;
	unsigned device_size_set:1;
	unsigned data_offset_set:1;
	unsigned seqno_set:1;
	unsigned pvid_set:1;
};

/*
 * command line input from --file
 */
struct metadata_file {
	const char *filename;
	char *text_buf;
	uint64_t text_size; /* bytes */
	uint32_t text_crc;
	char vgid_str[ID_STR_SIZE];
};

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

/* all sizes and offsets in bytes */

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

	if (!(new_buf = zalloc(new_len)))
		return;

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

/* all sizes and offsets in bytes */

static int _text_buf_parse(char *text_buf, uint64_t text_size, struct dm_config_tree **cft_out)
{
	struct dm_config_tree *cft;

	*cft_out = NULL;

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, NULL, 0))) {
		return 0;
	}

	if (!dm_config_parse(cft, text_buf, text_buf + text_size)) {
		config_destroy(cft);
		return 0;
	}

	*cft_out = cft;
	return 1;
}

/* all sizes and offsets in bytes */

static int _text_buf_parsable(char *text_buf, uint64_t text_size)
{
	struct dm_config_tree *cft = NULL;

	if (!_text_buf_parse(text_buf, text_size, &cft))
		return 0;

	config_destroy(cft);
	return 1;
}

#define MAX_LINE_CHECK 128

static void _copy_line(char *in, char *out, int *len, int linesize)
{
	int i;

	*len = 0;

	for (i = 0; i < linesize; i++) {
		out[i] = in[i];
		if ((in[i] == '\n') || (in[i] == '\0'))
			break;
	}
	*len = i+1;
}

/* all sizes and offsets in bytes */

static int _dump_all_text(struct cmd_context *cmd, struct settings *set, const char *tofile, struct device *dev,
			  int mda_num, uint64_t mda_offset, uint64_t mda_size, char *buf)
{
	FILE *fp = NULL;
	char line[MAX_LINE_CHECK];
	char vgname[NAME_LEN+1];
	char id_str[ID_STR_SIZE];
	char id_first[ID_STR_SIZE];
	char *text_buf;
	char *p;
	uint32_t buf_off; /* offset with buf which begins with mda_header, bytes */
	uint32_t buf_off_first = 0;
	uint32_t seqno;
	uint32_t crc;
	uint64_t text_size; /* bytes */
	uint64_t meta_size; /* bytes */
	int print_count = 0;
	int one_found = 0;
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

		if (one_found)
			break;

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
		 * user specified metadata in one location
		 */
		if (set->metadata_offset_set && (set->metadata_offset != (mda_offset + buf_off))) {
			count++;
			continue;
		}
		if (set->metadata_offset_set)
			one_found = 1;

		/*
		 * copy line of possible metadata to check for vgname
		 */
		memset(line, 0, sizeof(line));
		_copy_line(p, line, &len, sizeof(line));
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
		_copy_line(p, line, &len, sizeof(line));
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
		_copy_line(p, line, &len, sizeof(line));
		p += len;

		if (strncmp(line, "seqno = ", 8)) {
			count++;
			continue;
		}
		if (sscanf(line, "seqno = %u", &seqno) != 1) {
			count++;
			continue;
		}

		/*
		 * user specified metadata with one seqno
		 * (this is not good practice since multiple old copies of metadata
		 * can have the same seqno; this is mostly to simplify testing)
		 */
		if (set->seqno_set && (set->seqno != seqno)) {
			count++;
			continue;
		}
		if (set->seqno_set)
			one_found = 1;

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
			log_warn("WARNING: unexpected terminating bytes for metadata at %llu", (unsigned long long)(mda_offset + buf_off));

		if (arg_is_set(cmd, verbose_ARG)) {
			char *str1, *str2;
			if ((str1 = strstr(text_buf, "description = "))) {
				memset(line, 0, sizeof(line));
				_copy_line(str1, line, &len, sizeof(line));
				if ((p = strchr(line, '\n')))
					*p = '\0';
				log_print("%s", line);
			}
			if (str1 && (str2 = strstr(str1, "creation_time = "))) {
				memset(line, 0, sizeof(line));
				_copy_line(str2, line, &len, sizeof(line));
				if ((p = strchr(line, '\n')))
					*p = '\0';
				log_print("%s\n", line);
			}
		}

		if (fp) {
			if (print_count++)
				fprintf(fp, "\n--\n");
			fprintf(fp, "%s", text_buf);
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

/* all sizes and offsets in bytes */

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
 * all sizes and offsets in bytes
 *
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
 * all sizes and offsets in bytes
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
		log_error("Failed to create file %s", tofile);
		free(meta_buf);
		return 0;
	}

	fwrite(meta_buf, mda_size - 512, 1, fp);

	free(meta_buf);

	if (fflush(fp))
		stack;
	if (fclose(fp))
		stack;
	return 1;
}

/* all sizes and offsets in bytes */

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
	if (bad)
		return 0;
	return 1;
}

/* all sizes and offsets in bytes */

static int _dump_label_and_pv_header(struct cmd_context *cmd, uint64_t labelsector, struct device *dev,
				     int print_fields,
				     int *found_label,
				     uint64_t *mda1_offset, uint64_t *mda1_size,
				     uint64_t *mda2_offset, uint64_t *mda2_size,
				     int *mda_count_out)
{
	char buf[512];
	char str[256];
	struct label_header *lh;
	struct pv_header *pvh;
	struct pv_header_extension *pvhe;
	struct disk_locn *dlocn;
	uint64_t lh_offset;     /* bytes */
	uint64_t pvh_offset;    /* bytes */
	uint64_t pvhe_offset;   /* bytes */
	uint64_t dlocn_offset;  /* bytes */
	uint64_t tmp;
	int mda_count = 0;
	int bad = 0;
	int di;

	lh_offset = labelsector * 512; /* from start of disk */

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

	if (bad)
		return 0;
	return 1;
}

/*
 * all sizes and offsets in bytes
 *
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

static int _dump_mda_header(struct cmd_context *cmd, struct settings *set,
			    int print_fields, int print_metadata, int print_area,
			    const char *tofile,
			    struct device *dev,
			    uint64_t mda_offset, uint64_t mda_size,
			    uint32_t *checksum0_ret,
			    int *found_header)
{
	char buf[512];
	char str[256];
	char *mda_buf;
	struct mda_header *mh;
	struct raw_locn *rlocn0, *rlocn1;
	uint64_t rlocn0_offset, rlocn1_offset;
	uint64_t meta_offset = 0; /* bytes */
	uint64_t meta_size = 0;   /* bytes */
	uint32_t meta_checksum = 0;
	int mda_num = (mda_offset == 4096) ? 1 : 2;
	int bad = 0;

	*checksum0_ret = 0; /* checksum from raw_locn[0] */

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
		if (!(mda_buf = zalloc(mda_size)))
			goto_out;

		if (!dev_read_bytes(dev, mda_offset, mda_size, mda_buf)) {
			log_print("CHECK: failed to read metadata area at offset %llu size %llu",
				  (unsigned long long)mda_offset, (unsigned long long)mda_size);
			bad++;
			free(mda_buf);
			goto out;
		}

		_dump_all_text(cmd, set, tofile, dev, mda_num, mda_offset, mda_size, mda_buf);
		free(mda_buf);
	}

	/* Should we also check text metadata if it exists in rlocn1? */
 out:
	if (bad)
		return 0;
	return 1;
}

/* all sizes and offsets in bytes */

static int _dump_headers(struct cmd_context *cmd, const char *dump, struct settings *set,
			 uint64_t labelsector, struct device *dev)
{
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0; /* bytes */
	uint32_t mda1_checksum, mda2_checksum;
	int mda_count = 0;
	int bad = 0;

	if (!_dump_label_and_pv_header(cmd, labelsector, dev, 1, NULL,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count))
		bad++;

	if (!mda_count) {
		log_print("zero metadata copies");
		return 1;
	}

	/*
	 * The first mda is always 4096 bytes from the start of the device.
	 */
	if (!_dump_mda_header(cmd, set, 1, 0, 0, NULL, dev, 4096, mda1_size, &mda1_checksum, NULL))
		bad++;

	if (mda2_offset) {
		if (!_dump_mda_header(cmd, set, 1, 0, 0, NULL, dev, mda2_offset, mda2_size, &mda2_checksum, NULL))
			bad++;

		/* This probably indicates that one was committed and the other not. */
		if (mda1_checksum && mda2_checksum && (mda1_checksum != mda2_checksum))
			log_print("CHECK: mdas have different raw_locn[0].checksum values");
	}

	if (bad) {
		log_error("Found bad header or metadata values.");
		return 0;
	}
	return 1;
}

/* all sizes and offsets in bytes */

static int _dump_metadata(struct cmd_context *cmd, const char *dump, struct settings *set,
			 uint64_t labelsector, struct device *dev,
			 int print_metadata, int print_area)
{
	const char *tofile = NULL;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0; /* bytes */
	uint32_t mda1_checksum, mda2_checksum;
	int mda_count = 0;
	int mda_num = 1;
	int bad = 0;

	if (arg_is_set(cmd, file_ARG)) {
		if (!(tofile = arg_str_value(cmd, file_ARG, NULL)))
			return 0;
	}

	if (set->mda_num)
		mda_num = set->mda_num;
	else if (arg_is_set(cmd, pvmetadatacopies_ARG))
		mda_num = arg_int_value(cmd, pvmetadatacopies_ARG, 1);

	if (!_dump_label_and_pv_header(cmd, labelsector, dev, 0, NULL,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count))
		bad++;

	if (!mda_count) {
		log_print("zero metadata copies");
		return 1;
	}

	/*
	 * The first mda is always 4096 bytes from the start of the device.
	 */
	if (mda_num == 1) {
		if (!_dump_mda_header(cmd, set, 0, print_metadata, print_area, tofile, dev, 4096, mda1_size, &mda1_checksum, NULL))
			bad++;
	} else if (mda_num == 2) {
		if (!mda2_offset) {
			log_print("CHECK: second mda not found");
			bad++;
		} else {
			if (!_dump_mda_header(cmd, set, 0, print_metadata, print_area, tofile, dev, mda2_offset, mda2_size, &mda2_checksum, NULL))
				bad++;
		}
	}

	if (bad) {
		log_error("Found bad header or metadata values.");
		return 0;
	}
	return 1;
}

/* all sizes and offsets in bytes */

static int _dump_found(struct cmd_context *cmd, struct settings *set, uint64_t labelsector, struct device *dev)
{
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0; /* bytes */
	uint32_t mda1_checksum = 0, mda2_checksum = 0;
	int found_label = 0, found_header1 = 0, found_header2 = 0;
	int mda_count = 0;
	int bad = 0;

	if (!_dump_label_and_pv_header(cmd, labelsector, dev, 0, &found_label,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count))
		bad++;

	if (found_label && mda1_offset) {
		if (!_dump_mda_header(cmd, set, 0, 0, 0, NULL, dev, 4096, mda1_size, &mda1_checksum, &found_header1))
			bad++;
	}

	if (found_label && mda2_offset) {
		if (!_dump_mda_header(cmd, set, 0, 0, 0, NULL, dev, mda2_offset, mda2_size, &mda2_checksum, &found_header2))
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
 * all sizes and offsets in bytes (except dev_sectors from dev_get_size)
 *
 * Look for metadata text in common locations, without using any headers
 * (pv_header/mda_header) to find the location, since the headers may be
 * zeroed/damaged.
 */

static int _dump_search(struct cmd_context *cmd, const char *dump, struct settings *set,
			uint64_t labelsector, struct device *dev)
{
	const char *tofile = NULL;
	char *buf;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0; /* bytes */
	uint64_t mda_offset, mda_size; /* bytes */
	int mda_count = 0;
	int mda_num = 1;

	if (arg_is_set(cmd, file_ARG)) {
		if (!(tofile = arg_str_value(cmd, file_ARG, NULL)))
			return_0;
	}

	if (set->mda_num)
		mda_num = set->mda_num;
	else if (arg_is_set(cmd, pvmetadatacopies_ARG))
		mda_num = arg_int_value(cmd, pvmetadatacopies_ARG, 1);

	_dump_label_and_pv_header(cmd, labelsector, dev, 0, NULL,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count);

	/*
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
	if (set->mda_offset_set && set->mda_size_set) {
		mda_offset = set->mda_offset;
		mda_size = set->mda_size;
	} else if (mda_num == 1) {
		mda_offset = 4096;
		mda_size = ONE_MB_IN_BYTES - 4096;
	} else if (mda_num == 2) {
		uint64_t dev_sectors = 0;
		uint64_t dev_bytes;
		uint64_t extra_bytes;

		if (dev_get_size(dev, &dev_sectors))
			stack;

		dev_bytes = dev_sectors * 512;
		extra_bytes = dev_bytes % ONE_MB_IN_BYTES;

		if (dev_bytes < (2 * ONE_MB_IN_BYTES))
			return_0;

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

	log_print("Searching for metadata at offset %llu size %llu",
		  (unsigned long long)mda_offset, (unsigned long long)mda_size);

	if (!(buf = zalloc(mda_size)))
		return_0;

	if (!dev_read_bytes(dev, mda_offset, mda_size, buf)) {
		log_print("CHECK: failed to read metadata area at offset %llu size %llu",
			   (unsigned long long)mda_offset, (unsigned long long)mda_size);
		free(buf);
		return 0;
	}

	_dump_all_text(cmd, set, tofile, dev, mda_num, mda_offset, mda_size, buf);

	free(buf);
	return 1;
}

/* all sizes and offsets in bytes */

static int _get_one_setting(struct cmd_context *cmd, struct settings *set, char *key, char *val)
{
	if (!strncmp(key, "metadata_offset", strlen("metadata_offset"))) {
		if (sscanf(val, "%llu", (unsigned long long *)&set->metadata_offset) != 1)
			goto_bad;
		set->metadata_offset_set = 1;
		return 1;
	}

	if (!strncmp(key, "seqno", strlen("seqno"))) {
		if (sscanf(val, "%u", &set->seqno) != 1)
			goto_bad;
		set->seqno_set = 1;
		return 1;
	}

	if (!strncmp(key, "backup_file", strlen("backup_file"))) {
		if ((set->backup_file = strdup(val)))
			return 1;
		return 0;
	}

	if (!strncmp(key, "mda_offset", strlen("mda_offset"))) {
		if (sscanf(val, "%llu", (unsigned long long *)&set->mda_offset) != 1)
			goto_bad;
		set->mda_offset_set = 1;
		return 1;
	}

	if (!strncmp(key, "mda_size", strlen("mda_size"))) {
		if (sscanf(val, "%llu", (unsigned long long *)&set->mda_size) != 1)
			goto_bad;
		set->mda_size_set = 1;
		return 1;
	}

	if (!strncmp(key, "mda2_offset", strlen("mda2_offset"))) {
		if (sscanf(val, "%llu", (unsigned long long *)&set->mda2_offset) != 1)
			goto_bad;
		set->mda2_offset_set = 1;
		return 1;
	}

	if (!strncmp(key, "mda2_size", strlen("mda2_size"))) {
		if (sscanf(val, "%llu", (unsigned long long *)&set->mda2_size) != 1)
			goto_bad;
		set->mda2_size_set = 1;
		return 1;
	}

	if (!strncmp(key, "device_size", strlen("device_size"))) {
		if (sscanf(val, "%llu", (unsigned long long *)&set->device_size) != 1)
			goto_bad;
		set->device_size_set = 1;
		return 1;
	}

	if (!strncmp(key, "data_offset", strlen("data_offset"))) {
		if (sscanf(val, "%llu", (unsigned long long *)&set->data_offset) != 1)
			goto_bad;
		set->data_offset_set = 1;
		return 1;
	}

	if (!strncmp(key, "pv_uuid", strlen("pv_uuid"))) {
		if (strchr(val, '-') && (strlen(val) == 32)) {
			memcpy(&set->pvid, val, 32);
			set->pvid_set = 1;
			return 1;
		} else if (id_read_format_try(&set->pvid, val)) {
			set->pvid_set = 1;
			return 1;
		} else {
			log_error("Failed to parse UUID from pv_uuid setting.");
			goto bad;
		}
	}

	if (!strncmp(key, "mda_num", strlen("mda_num"))) {
		if (sscanf(val, "%u", (int *)&set->mda_num) != 1)
			goto_bad;
		return 1;
	}
bad:
	log_error("Invalid setting: %s", key);
	return 0;
}

static int _get_settings(struct cmd_context *cmd, struct settings *set)
{
	struct arg_value_group_list *group;
	const char *str;
	char key[64];
	char val[64];
	int num;
	int pos;

	/*
	 * "grouped" means that multiple --settings options can be used.
	 * Each option is also allowed to contain multiple key = val pairs.
	 */

	dm_list_iterate_items(group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(group->arg_values, settings_ARG))
			continue;

		if (!(str = grouped_arg_str_value(group->arg_values, settings_ARG, NULL)))
			break;

		pos = 0;

		while (pos < strlen(str)) {
			/* scan for "key1=val1 key2 = val2  key3= val3" */

			memset(key, 0, sizeof(key));
			memset(val, 0, sizeof(val));

			if (sscanf(str + pos, " %63[^=]=%63s %n", key, val, &num) != 2) {
				log_error("Invalid setting at: %s", str+pos);
				return 0;
			}

			pos += num;

			if (!_get_one_setting(cmd, set, key, val))
				return_0;
		}
	}

	return 1;
}

/*
 * pvck --repairtype label_header
 *
 * Writes new label_header without changing pv_header fields.
 * All constant values except for recalculated crc.
 *
 * all sizes and offsets in bytes
 */

static int _repair_label_header(struct cmd_context *cmd, const char *repair,
			        struct settings *set, uint64_t labelsector, struct device *dev)
{
	char buf[512];
	struct label_header *lh;
	struct pv_header *pvh;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0; /* bytes */
	uint64_t lh_offset;  /* bytes */
	uint64_t pvh_offset; /* bytes */
	uint32_t crc;
	int mda_count;
	int found_label = 0;

	lh_offset = labelsector * 512; /* from start of disk */

	_dump_label_and_pv_header(cmd, labelsector, dev, 0, &found_label,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count);

	if (!found_label) {
		log_warn("WARNING: No LVM label found on %s.  It may not be an LVM device.", dev_name(dev));
		if (!arg_count(cmd, yes_ARG) &&
		    yes_no_prompt("Write LVM header to device? ") == 'n')
			return 0;
	}

	if (!dev_read_bytes(dev, lh_offset, 512, buf)) {
	        log_error("Failed to read label_header at %llu", (unsigned long long)lh_offset);
	        return 0;
	}

	lh = (struct label_header *)buf;
	pvh = (struct pv_header *)(buf + 32);
	pvh_offset = lh_offset + 32; /* from start of disk */

	/* sanity check */
	if ((void *)pvh != (void *)(buf + pvh_offset - lh_offset)) {
	        log_error("Problem with pv_header offset calculation");
	        return 0;
	}

	memcpy(lh->id, LABEL_ID, sizeof(lh->id));
	memcpy(lh->type, LVM2_LABEL, sizeof(lh->type));
	lh->sector_xl =  xlate64(labelsector);
	lh->offset_xl = xlate32(32);

	crc = calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl,
	               LABEL_SIZE - ((uint8_t *) &lh->offset_xl - (uint8_t *) lh));

	lh->crc_xl = xlate32(crc);
       
	log_print("Writing label_header.crc 0x%08x", crc);

	if (arg_is_set(cmd, test_ARG)) {
		log_warn("Skip writing in test mode.");
		return 1;
	}

	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt("Write new LVM header to %s? ", dev_name(dev)) == 'n')
		return 0;

	if (!dev_write_bytes(dev, lh_offset, 512, buf)) {
	        log_error("Failed to write new header");
	        return 0;
	}
	return 1;
}

static int _get_pv_info_from_metadata(struct cmd_context *cmd, struct settings *set,
				     struct device *dev,
				     struct pv_header *pvh, int found_label,
				     char *text_buf, uint64_t text_size,
				     char *pvid,
				     uint64_t *device_size_sectors,
				     uint64_t *pe_start_sectors)
{
	int8_t pvid_cur[ID_LEN+1];  /* found in existing pv_header */
	int8_t pvid_set[ID_LEN+1];  /* set by user in --settings */
	int8_t pvid_use[ID_LEN+1];  /* the pvid chosen to use */
	int pvid_cur_valid = 0;     /* pvid_cur is valid */
	int pvid_use_valid = 0;     /* pvid_use is valid */
	struct dm_config_tree *cft = NULL;
	struct volume_group *vg = NULL;
	struct pv_list *pvl;

	memset(pvid_cur, 0, sizeof(pvid_cur));
	memset(pvid_set, 0, sizeof(pvid_set));
	memset(pvid_use, 0, sizeof(pvid_use));

	/*
	 * Check if there's a valid existing PV UUID at the expected location.
	 */
	if (!id_read_format_try((struct id *)&pvid_cur, (char *)&pvh->pv_uuid))
		memset(&pvid_cur, 0, ID_LEN);
	else {
		memcpy(&pvid_use, &pvid_cur, ID_LEN);
		pvid_use_valid = 1;
		pvid_cur_valid = 1;
	}

	if (set->pvid_set) {
		memcpy(&pvid_set, &set->pvid, ID_LEN);
		memcpy(&pvid_use, &pvid_set, ID_LEN);
		pvid_use_valid = 1;
	}

	if (pvid_cur_valid && set->pvid_set && memcmp(&pvid_cur, &pvid_set, ID_LEN)) {
		log_warn("WARNING: existing PV UUID %s does not match pv_uuid setting %s.",
			 (char *)&pvid_cur, (char *)&pvid_set);

		memcpy(&pvid_use, &pvid_set, ID_LEN);
		pvid_use_valid = 1;
	}

	if (!_text_buf_parse(text_buf, text_size, &cft)) {
		log_error("Invalid metadata file.");
		return 0;
	}

	if (!(vg = vg_from_config_tree(cmd, cft))) {
		config_destroy(cft);
		log_error("Invalid metadata file.");
		return 0;
	}

	config_destroy(cft);

	/*
	 * If pvid_use is set, look for metadata PV section with matching PV UUID.
	 * Otherwise, look for metadata PV section with device name matching dev.
	 *
	 * pvid_use will be empty if there's no valid UUID in the existing
	 * pv_header, and the user did not specify a UUID in --settings.
	 *
	 * Choosing the PV UUID based only on a matching device name is somewhat
	 * weak since device names are dynamic, but we do scan devs to verify the
	 * chosen PV UUID is not in use elsewhere, which should avoid most of the
	 * risk of picking a wrong UUID.
	 */
	if (!pvid_use_valid) {
		dm_list_iterate_items(pvl, &vg->pvs) {
			if (!strcmp(pvl->pv->device_hint, dev_name(dev)))
				goto copy_pv;
		}
	} else {
		dm_list_iterate_items(pvl, &vg->pvs) {
			if (id_equal(&pvl->pv->id, (struct id *)&pvid_use))
				goto copy_pv;
		}
	}

	release_vg(vg);

	/*
	 * Don't know what PV UUID to use, possibly:
	 * . the user set a PV UUID that does not exist in the metadata file
	 * . the UUID in the existing pv_header does not exist in the metadata file
	 * . the metadata has no PV with a device name hint matching this device
	 */
	if (set->pvid_set)
		log_error("PV UUID %s not found in metadata file.", (char *)&pvid_set);
	else if (pvid_cur_valid)
		log_error("PV UUID %s in existing pv_header not found in metadata file.", (char *)&pvid_cur);
	else if (!pvid_use_valid)
		log_error("PV name %s not found in metadata file.", dev_name(dev));

	log_error("No valid PV UUID, specify a PV UUID from metadata in --settings.");
	return 0;

 copy_pv:
	*device_size_sectors = pvl->pv->size;
	*pe_start_sectors = pvl->pv->pe_start;
	memcpy(pvid, &pvl->pv->id, ID_LEN);

	release_vg(vg);
	return 1;
}

/*
 * Checking for mda1 is simple because it's always at the same location,
 * and when a PV is set to use zero metadata areas, this space is just
 * unused.  We could look for any surviving metadata text in mda1
 * containing the VG UUID to confirm that this PV has been used for
 * metadata, but if the start of the disk has been zeroed, then we
 * may not find any.
 */
static int _check_for_mda1(struct cmd_context *cmd, struct device *dev)
{
	char buf[512];
	struct mda_header *mh;

	if (!dev_read_bytes(dev, 4096, 512, buf))
		return_0;

	mh = (struct mda_header *)buf;

	if (!memcmp(mh->magic, FMTT_MAGIC, sizeof(mh->magic)))
		return 1;
	return 0;
}

/*
 * Checking for mda2 is more complicated.  Very often PVs will not use
 * a second mda2, and the location is not quite as predictable.  Also,
 * if we mistakenly conclude that an mda2 belongs on the PV, we'd end
 * up writing into the data area.
 *
 * all sizes and offsets in bytes
 */
static int _check_for_mda2(struct cmd_context *cmd, struct device *dev,
			   uint64_t device_size, struct metadata_file *mf,
			   uint64_t *mda2_offset, uint64_t *mda2_size)
{
	struct mda_header *mh;
	char buf2[256];
	char *buf;
	uint64_t mda_offset, mda_size, extra_bytes; /* bytes */
	int i, found = 0;

	if (device_size < (2 * ONE_MB_IN_BYTES))
		return_0;

	extra_bytes = device_size % ONE_MB_IN_BYTES;
	mda_offset = device_size - extra_bytes - ONE_MB_IN_BYTES;
	mda_size = device_size - mda_offset;

	if (!(buf = malloc(mda_size)))
		return_0;

	if (!dev_read_bytes(dev, mda_offset, mda_size, buf))
		goto fail;

	mh = (struct mda_header *)buf;

	/*
	 * To be certain this is really an mda_header before writing it,
	 * require that magic, version and start are all correct.
	 */

	if (memcmp(mh->magic, FMTT_MAGIC, sizeof(mh->magic)))
		goto fail;

	if (xlate32(mh->version) != FMTT_VERSION) {
		log_print("Skipping mda2 (wrong mda_header.version)");
		goto fail;
	}

	if (xlate64(mh->start) != mda_offset) {
		log_print("Skipping mda2 (wrong mda_header.start)");
		goto fail;
	}

	/*
	 * Search text area for an instance of current metadata before enabling
	 * mda2, in case this mda_header is from a previous generation PV and
	 * is not actually used by the current PV.  An mda_header and metadata
	 * area from a previous PV (in a previous VG) that used mda2 might
	 * still exist, while the current PV does not use an mda2.
	 *
	 * Search for the vgid in the first 256 bytes at each 512 byte boundary
	 * in the first half of the metadata area.
	 */
	for (i = 0; i < (mda_size / 1024); i++) {
		memcpy(buf2, buf + 512 + (i * 512), sizeof(buf2));

		if (strstr(buf2, mf->vgid_str)) {
			log_print("Found mda2 header at offset %llu size %llu",
				  (unsigned long long)mda_offset, (unsigned long long)mda_size);
			*mda2_offset = mda_offset;
			*mda2_size = mda_size;
			found = 1;
			break;
		}
	}
	if (!found) {
		log_print("Skipping mda2 (no matching VG UUID in metadata area)");
		goto fail;
	}

	free(buf);
	return 1;

 fail:
	free(buf);
	*mda2_offset = 0;
	*mda2_size = 0;
	return 0;
}

/*
 * pvck --repairtype pv_header --file input --settings
 *
 * Writes new pv_header and label_header.
 *
 * pv_header.pv_uuid
 * If a uuid is given in --settings, that is used.
 * Else if existing pv_header has a valid uuid, that is used.
 * Else if the metadata file has a matching device name, that uuid is used.
 *
 * pv_header.device_size
 * Use device size from metadata file.
 *
 * pv_header.disk_locn[0].offset (data area start)
 * Use pe_start from metadata file.
 *
 * pv_header.disk_locn[2].offset/size (first metadata area)
 * offset always 4096.  size is pe_start - offset.
 *
 * pv_header.disk_locn[3].offset/size (second metadata area)
 * Look for existing mda_header at expected offset, and if
 * found use that value.  Otherwise second mda is not used.
 *
 * The size/offset variables in sectors have a _sectors suffix,
 * any other size/offset variables in bytes.
 */

static int _repair_pv_header(struct cmd_context *cmd, const char *repair,
			     struct settings *set, struct metadata_file *mf,
			     uint64_t labelsector, struct device *dev)
{
	char head_buf[512];
	int8_t pvid[ID_LEN+1];
	struct device *dev_with_pvid = NULL;
	struct label_header *lh;
	struct pv_header *pvh;
	struct pv_header_extension *pvhe;
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0; /* bytes */
	uint64_t lh_offset;               /* in bytes, from start of disk */
	uint64_t device_size = 0;         /* in bytes, as stored in pv_header */
	uint64_t device_size_sectors = 0; /* in sectors, as stored in metadata */
	uint64_t get_size_sectors = 0;    /* in sectors, as dev_get_size returns */
	uint64_t get_size = 0;            /* in bytes */
	uint64_t pe_start_sectors;        /* in sectors, as stored in metadata */
	uint64_t data_offset;             /* in bytes, as stored in pv_header */
	uint32_t head_crc;
	int mda_count;
	int found_label = 0;
	int di;

	memset(&pvid, 0, ID_LEN+1);

	lh_offset = labelsector * 512; /* from start of disk */

	if (!dev_get_size(dev, &get_size_sectors))
		log_warn("WARNING: Cannot get device size.");
	get_size = get_size_sectors << SECTOR_SHIFT;

	_dump_label_and_pv_header(cmd, labelsector, dev, 0, &found_label,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count);

	/*
	 * The header sector may have been zeroed, or the user may have
	 * accidentally given the wrong device.
	 */
	if (!found_label)
		log_warn("WARNING: No LVM label found on %s.  It may not be an LVM device.", dev_name(dev));

	/*
	 * The PV may have had no metadata areas, or one, or two.
	 *
	 * Try to avoid writing new metadata areas where they didn't exist
	 * before.  Writing mda1 when it didn't exist previously would not be
	 * terrible since the space is unused anyway, but wrongly writing mda2
	 * could end up in the data area.
	 *
	 * When the pv_header has no mda1 or mda2 locations, check for evidence
	 * of prior mda headers for mda1 and mda2.
	 *
	 * When the pv_header has an mda1 location and no mda2 location, just
	 * use mda1 and don't look for mda2 (unless requested by user setting)
	 * since it probably did not exist.  (It's very unlikely that only the
	 * mda2 location was zeroed in the pv_header.)
	 */
	if (!mda_count && !mda1_offset && !mda2_offset) {
		if (_check_for_mda1(cmd, dev))
			mda_count = 1;

		if (_check_for_mda2(cmd, dev, get_size, mf, &mda2_offset, &mda2_size))
			mda_count = 2;
	}

	/*
	 * The PV may have had zero metadata areas (not common), or the
	 * pv_header and the mda1 header at 4096 may have been zeroed
	 * (more likely).  Ask the user if metadata in mda1 should be
	 * included; it would usually be yes.  To repair a PV and use
	 * zero metadata areas, require the user to specify
	 * --settings "mda_offset=0 mda_size=0".
	 *
	 * NOTE: mda1 is not written by repair pv_header, this will only
	 * include a pointer to mda1 in the pv_header so that a subsequent
	 * repair metadata will use that to write an mda_header and metadata.
	 */
	if (!mda_count && set->mda_offset_set && set->mda_size_set &&
	    !set->mda_offset && !set->mda_size) {
		log_warn("WARNING: PV will have no metadata with zero metadata areas.");

	} else if (!mda_count) {
		log_warn("WARNING: no previous metadata areas found on device.");

		if (arg_count(cmd, yes_ARG) ||
		    yes_no_prompt("Should a metadata area be included? ") == 'y') {
			/* mda1_offset/mda1_size are set below */
			mda_count = 1;
		} else {
			log_error("To repair with zero metadata areas, use --settings \"mda_offset=0 mda_size=0\".");
			goto fail;
		}
	}

	/*
	 * The user has provided offset or size for mda2.  This would
	 * usually be done when these values do not exist on disk,
	 * but if mda2 *is* found on disk, ensure it agrees with the
	 * user's setting.
	 */
	if (mda_count && (set->mda2_offset_set || set->mda2_size_set)) {
		if (mda2_offset && (mda2_offset != set->mda2_offset)) {
			log_error("mda2_offset setting %llu does not match mda2_offset found on disk %llu.",
				 (unsigned long long)set->mda2_offset, (unsigned long long)mda2_offset);
			goto fail;
		}
		if (mda2_size && (mda2_size != set->mda2_size)) {
			log_error("mda2_size setting %llu does not match mda2_size found on disk %llu.",
				 (unsigned long long)set->mda2_size, (unsigned long long)mda2_size);
			goto fail;
		}
		mda2_offset = set->mda2_offset;
		mda2_size = set->mda2_size;
		mda_count = 2;
	}

	/*
	 * The header sector is read into this buffer.
	 * This same buffer is modified and written back.
	 */
	if (!dev_read_bytes(dev, lh_offset, 512, head_buf)) {
	        log_error("Failed to read label_header at %llu", (unsigned long long)lh_offset);
		goto fail;
	}

	lh = (struct label_header *)head_buf;
	pvh = (struct pv_header *)(head_buf + 32);

	/*
	 * Metadata file is not needed if user provides pvid/device_size/data_offset.
	 * All values in settings are in bytes.
	 */
	if (set->device_size_set && set->pvid_set && set->data_offset_set && !mf->filename) {
		device_size = set->device_size;
		pe_start_sectors = set->data_offset >> SECTOR_SHIFT;
		memcpy(&pvid, &set->pvid, ID_LEN);

		if (get_size && (get_size != device_size)) {
			log_warn("WARNING: device_size setting %llu bytes does not match device size %llu bytes.",
				 (unsigned long long)set->device_size, (unsigned long long)get_size);
		}
		goto scan;
	}

	if (!mf->filename) {
		log_error("Metadata input file is needed for pv_header info.");
		log_error("See pvck --dump to locate and create a metadata file.");
		goto fail;
	}

	/*
	 * Look in the provided copy of VG metadata for info that determines
	 * pv_header fields.
	 *
	 * pv<N> {
	 * 	id = <uuid>
	 * 	device = <path>  # device path hint, set when metadata was last written
	 * 	...
	 * 	dev_size = <num> # in 512 sectors
	 * 	pe_start = <num> # in 512 sectors
	 * }
	 *
	 * Select the right pv entry by matching an existing pv uuid, or the
	 * current device name to the device path hint.  Take the pv uuid,
	 * dev_size and pe_start from the metadata to use in the pv_header.
	 */
	if (!_get_pv_info_from_metadata(cmd, set, dev, pvh, found_label,
					mf->text_buf, mf->text_size, (char *)&pvid,
					&device_size_sectors, &pe_start_sectors))
		goto fail;

	/*
	 * In pv_header, device_size is bytes, but in metadata dev_size is in sectors.
	 */
	device_size = device_size_sectors << SECTOR_SHIFT;

 scan:
	/*
	 * Read all devs to verify the pvid that will be written does not exist
	 * on another device.
	 */
	if (!label_scan_for_pvid(cmd, (char *)&pvid, &dev_with_pvid)) {
		log_error("Failed to scan devices to check PV UUID.");
		goto fail;
	}

	if (dev_with_pvid && (dev_with_pvid != dev)) {
		log_error("Cannot use PV UUID %s which exists on %s", (char *)&pvid, dev_name(dev_with_pvid));
		goto fail;
	}

	/*
	 * Set new label_header and pv_header fields.
	 */

	/* set label_header (except crc) */
	memcpy(lh->id, LABEL_ID, sizeof(lh->id));
	memcpy(lh->type, LVM2_LABEL, sizeof(lh->type));
	lh->sector_xl =  xlate64(labelsector);
	lh->offset_xl = xlate32(32);

	/* set pv_header */
	memcpy(pvh->pv_uuid, &pvid, ID_LEN);
	pvh->device_size_xl = xlate64(device_size);

	/* set data area location */
	data_offset = (pe_start_sectors << SECTOR_SHIFT);
	pvh->disk_areas_xl[0].offset = xlate64(data_offset);
	pvh->disk_areas_xl[0].size = 0;

	/* set end of data areas */
	pvh->disk_areas_xl[1].offset = 0;
	pvh->disk_areas_xl[1].size = 0;

	di = 2;

	/* set first metadata area location */
	if (mda_count > 0) {
		mda1_offset = 4096;
		mda1_size = (pe_start_sectors << SECTOR_SHIFT) - 4096;
		pvh->disk_areas_xl[di].offset = xlate64(mda1_offset);
		pvh->disk_areas_xl[di].size = xlate64(mda1_size);
		di++;
	}

	/* set second metadata area location */
	if (mda_count > 1) {
		pvh->disk_areas_xl[di].offset = xlate64(mda2_offset);
		pvh->disk_areas_xl[di].size = xlate64(mda2_size);
		di++;
	}

	/* set end of metadata areas */
	pvh->disk_areas_xl[di].offset = 0;
	pvh->disk_areas_xl[di].size = 0;
	di++;

	/* set pv_header_extension */
	pvhe = (struct pv_header_extension *)((char *)pvh + sizeof(struct pv_header) + (di * sizeof(struct disk_locn)));
	pvhe->version = xlate32(PV_HEADER_EXTENSION_VSN);
	pvhe->flags = xlate32(PV_EXT_USED);
	pvhe->bootloader_areas_xl[0].offset = 0;
	pvhe->bootloader_areas_xl[0].size = 0;

	head_crc = calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl,
			    LABEL_SIZE - ((uint8_t *) &lh->offset_xl - (uint8_t *) lh));

	/* set label_header crc (last) */
	lh->crc_xl = xlate32(head_crc);

	/*
	 * Write the updated header sector.
	 */

	log_print("Writing label_header.crc 0x%08x pv_header uuid %s device_size %llu",
		  head_crc, (char *)&pvid, (unsigned long long)device_size);

	log_print("Writing data_offset %llu mda1_offset %llu mda1_size %llu mda2_offset %llu mda2_size %llu",
		  (unsigned long long)data_offset,
		  (unsigned long long)mda1_offset,
		  (unsigned long long)mda1_size,
		  (unsigned long long)mda2_offset,
		  (unsigned long long)mda2_size);

	if (arg_is_set(cmd, test_ARG)) {
		log_warn("Skip writing in test mode.");
		return 1;
	}

	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt("Write new LVM header to %s? ", dev_name(dev)) == 'n')
		goto fail;

	if (!dev_write_bytes(dev, lh_offset, 512, head_buf)) {
	        log_error("Failed to write new header");
		goto fail;
	}

	return 1;
fail:
	return 0;
}

/* all sizes and offsets in bytes */

static int _update_mda(struct cmd_context *cmd, struct metadata_file *mf, struct device *dev,
		       int mda_num, uint64_t mda_offset, uint64_t mda_size)
{
	char *buf[512];
	struct mda_header *mh;
	struct raw_locn *rlocn0, *rlocn1;
	uint64_t max_size;
	uint64_t text_offset;
	uint32_t crc;

	max_size = ((mda_size - 512) / 2) - 512;
	if (mf->text_size > mda_size) {
		log_error("Metadata text %llu too large for mda_size %llu max %llu",
			  (unsigned long long)mf->text_size,
			  (unsigned long long)mda_size,
			  (unsigned long long)max_size);
		goto fail;
	}

	if (!dev_read_bytes(dev, mda_offset, 512, buf)) {
		log_print("CHECK: failed to read mda_header_%d at %llu",
			  mda_num, (unsigned long long)mda_offset);
		goto fail;
	}

	text_offset = mda_offset + 512;

	mh = (struct mda_header *)buf;
	memcpy(mh->magic, FMTT_MAGIC, sizeof(mh->magic));
	mh->version = xlate32(FMTT_VERSION);
	mh->start = xlate64(mda_offset);
	mh->size = xlate64(mda_size);

	rlocn0 = mh->raw_locns;
	rlocn0->flags = 0;
	rlocn0->offset = xlate64(512); /* text begins 512 from start of mda_header */
	rlocn0->size = xlate64(mf->text_size);
	rlocn0->checksum = xlate32(mf->text_crc);

	rlocn1 = (struct raw_locn *)((char *)mh->raw_locns + 24);
	rlocn1->flags = 0;
	rlocn1->offset = 0;
	rlocn1->size = 0;
	rlocn1->checksum = 0;

	crc = calc_crc(INITIAL_CRC, (uint8_t *)mh->magic,
		       MDA_HEADER_SIZE - sizeof(mh->checksum_xl));
	mh->checksum_xl = xlate32(crc);

	log_print("Writing metadata at %llu length %llu crc 0x%08x mda%d",
		  (unsigned long long)(mda_offset + 512),
		  (unsigned long long)mf->text_size, mf->text_crc, mda_num);

	log_print("Writing mda_header at %llu mda%d",
		  (unsigned long long)mda_offset, mda_num);

	if (arg_is_set(cmd, test_ARG)) {
		log_warn("Skip writing in test mode.");
		return 1;
	}

	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt("Write new LVM metadata to %s? ", dev_name(dev)) == 'n')
		goto fail;

	if (!dev_write_bytes(dev, text_offset, mf->text_size, mf->text_buf)) {
		log_error("Failed to write new mda text");
		goto fail;
	}

	if (!dev_write_bytes(dev, mda_offset, 512, buf)) {
		log_error("Failed to write new mda header");
		goto fail;
	}

	return 1;
 fail:
	return 0;
}

/*
 * pvck --repairtype metadata --file input --settings
 *
 * Writes new metadata into the text area and writes new
 * mda_header for it.  Requires valid mda locations in pv_header.
 * Metadata is written immediately after mda_header.
 *
 * all sizes and offsets in bytes
 */

static int _repair_metadata(struct cmd_context *cmd, const char *repair,
			    struct settings *set, struct metadata_file *mf,
			    uint64_t labelsector, struct device *dev)
{
	uint64_t mda1_offset = 0, mda1_size = 0, mda2_offset = 0, mda2_size = 0; /* bytes */
	int found_label = 0;
	int mda_count = 0;
	int mda_num;
	int bad = 0;

	mda_num = set->mda_num;

	if (!mf->filename) {
		log_error("Metadata input file is required.");
		return 0;
	}

	_dump_label_and_pv_header(cmd, labelsector, dev, 0, &found_label,
			&mda1_offset, &mda1_size, &mda2_offset, &mda2_size, &mda_count);

	if (!found_label) {
		log_error("No lvm label found on device.");
		log_error("See --repairtype pv_header to repair headers.");
		return 0;
	}

	if (!mda_count && set->mda_offset_set && set->mda_size_set &&
	    !set->mda_offset && !set->mda_size) {
		log_print("No metadata areas on device to repair.");
		return 1;
	}

	if (!mda_count) {
		log_error("No metadata areas found on device.");
		log_error("See --repairtype pv_header to repair headers.");
		return 0;
	}

	if ((mda_num == 1) && !mda1_offset) {
		log_error("No mda1 offset found.");
		log_error("See --repairtype pv_header to repair headers.");
		return 0;
	}

	if ((mda_num == 2) && !mda2_offset) {
		log_error("No mda2 offset found.");
		log_error("See --repairtype pv_header to repair headers.");
		return 0;
	}

	if ((!mda_num || mda_num == 1) && mda1_offset) {
		if (!_update_mda(cmd, mf, dev, 1, mda1_offset, mda1_size))
			bad++;
	}

	if ((!mda_num || mda_num == 2) && mda2_offset) {
		if (!_update_mda(cmd, mf, dev, 2, mda2_offset, mda2_size))
			bad++;
	}

	if (bad)
		return 0;

	return 1;
}

static void _strip_backup_line(char *line1, int len1, char *line2, int *len2)
{
	int copying = 0;
	int i, j = 0;

	for (i = 0; i < len1; i++) {
		if (line1[i] == '\0')
			break;

		if (line1[i] == '\n')
			break;

		/* omit tabs at start of line */
		if (!copying && (line1[i] == '\t'))
			continue;

		/* omit tabs and comment at end of line (can tabs occur without comment?) */
		if (copying && (line1[i] == '\t') && strchr(line1 + i, '#'))
			break;

		copying = 1;

		line2[j++] = line1[i];
	}

	line2[j++] = '\n';
	*len2 = j;
}

#define MAX_META_LINE 4096

/* all sizes and offsets in bytes */

static int _backup_file_to_raw_metadata(char *back_buf, uint64_t back_size,
					char **text_buf_out, uint64_t *text_size_out)
{
	char line[MAX_META_LINE];
	char line2[MAX_META_LINE];
	char *p, *text_buf;
	uint32_t text_pos, pre_len, back_pos, text_max;
	int len, len2, vgnamelen;

	text_max = back_size * 2;

	if (!(text_buf = zalloc(text_max)))
		return_0;

	p = back_buf;
	text_pos = 0;
	back_pos = 0;

	while (1) {
		if (back_pos >= back_size)
			break;

		memset(line, 0, sizeof(line));
		len = 0;

		_copy_line(p, line, &len, sizeof(line));
		p += len;
		back_pos += len;

		if (len < 3)
			continue;

		if (_check_vgname_start(line, &vgnamelen)) {
			/* vg name is first line of text_buf */
			memcpy(text_buf, line, len);
			text_pos = len;

			pre_len = back_pos - len;
			break;
		}
	}

	while (1) {
		if (back_pos >= back_size)
			break;

		memset(line, 0, sizeof(line));
		memset(line2, 0, sizeof(line2));
		len = 0;
		len2 = 0;

		_copy_line(p, line, &len, sizeof(line));

		if (line[0] == '\0')
			break;

		p += len;
		back_pos += len;

		/* shouldn't happen */
		if (text_pos + len > text_max)
			return_0;

		if (len == 1) {
			text_buf[text_pos++] = '\n';
			continue;
		}

		_strip_backup_line(line, len, line2, &len2);

		memcpy(text_buf + text_pos, line2, len2);
		text_pos += len2;
	}

	/* shouldn't happen */
	if (text_pos + pre_len + 3 > text_max)
		return_0;

	/* copy first pre_len bytes of back_buf into text_buf */
	memcpy(text_buf + text_pos, back_buf, pre_len);
	text_pos += pre_len;

	text_pos++; /* null termination */

	*text_size_out = text_pos;
	*text_buf_out = text_buf;

	return 1;
}

static int _is_backup_file(struct cmd_context *cmd, char *text_buf, uint64_t text_size)
{
	if ((text_buf[0] == '#') && !strncmp(text_buf, "# Generated", 11))
		return 1;
	return 0;
}

/* all sizes and offsets in bytes */

static int _dump_backup_to_raw(struct cmd_context *cmd, struct settings *set)
{
	const char *input = set->backup_file;
	const char *tofile = NULL;
	struct stat sb;
	char *back_buf, *text_buf;
	uint64_t back_size, text_size;
	int fd, rv;

	if (arg_is_set(cmd, file_ARG)) {
		if (!(tofile = arg_str_value(cmd, file_ARG, NULL)))
			return_0;
	}

	if (!input) {
		log_error("Set backup file in --settings backup_file=path");
		return 0;
	}

	if (!(fd = open(input, O_RDONLY))) {
		log_error("Cannot open file: %s", input);
		return 0;
	}

	if (fstat(fd, &sb)) {
		log_error("Cannot access file: %s", input);
		close(fd);
		return 0;
	}

	if (!(back_size = (uint64_t)sb.st_size)) {
		log_error("Empty file: %s", input);
		close(fd);
		return 0;
	}

	if (!(back_buf = zalloc(back_size))) {
		close(fd);
		return 0;
	}

	rv = read(fd, back_buf, back_size);
	if (rv != back_size) {
		log_error("Cannot read file: %s", input);
		close(fd);
		free(back_buf);
		return 0;
	}

	close(fd);

	if (!_is_backup_file(cmd, back_buf, back_size)) {
		log_error("File does not appear to contain a metadata backup.");
		free(back_buf);
		return 0;
	}

	if (!_backup_file_to_raw_metadata(back_buf, back_size, &text_buf, &text_size)) {
		free(back_buf);
		return_0;
	}

	free(back_buf);

	if (!tofile) {
		log_print("---");
		printf("%s\n", text_buf);
		log_print("---");
	} else {
		FILE *fp;
		if (!(fp = fopen(tofile, "wx"))) {
			log_error("Failed to create file %s", tofile);
			return 0;
		}

		fprintf(fp, "%s", text_buf);

		if (fflush(fp))
			stack;
		if (fclose(fp))
			stack;
	}

	return 1;
}

/* all sizes and offsets in bytes */

static int _check_metadata_file(struct cmd_context *cmd, struct metadata_file *mf,
				char *text_buf, int text_size)
{
	char *vgid;
	int namelen;

	if (text_size < NAME_LEN+1) {
		log_error("Invalid raw text metadata in file.  File size is too small.");
		return 0;
	}

	/*
	 * Using pvck --dump metadata output redirected to file may be a common
	 * mistake, so check and warn about that specifically.
	 */
	if (isspace(text_buf[0]) && isspace(text_buf[1]) && strstr(text_buf, "---")) {
		log_error("Invalid raw text metadata in file.");
		log_error("(pvck stdout is not valid input, see pvck -f.)");
		return 0;
	}

	/*
	 * Using a metadata backup file may be another common mistake.
	 */
	if ((text_buf[0] == '#') && !strncmp(text_buf, "# Generated", 11)) {
		log_error("Invalid raw text metadata in file.");
		log_error("(metadata backup file is not valid input.)");
		return 0;
	}

	if (text_buf[text_size-1] != '\0' ||
	    text_buf[text_size-2] != '\n' ||
	    text_buf[text_size-3] != '\n')
		log_warn("WARNING: unexpected final bytes of raw metadata, expected \\n\\n\\0.");

	if (_check_vgname_start(text_buf, &namelen)) {
		if (!(vgid = strstr(text_buf, "id = "))) {
			log_error("Invalid raw text metadata in file.  (No VG UUID found.)");
			return 0;
		}
		memcpy(mf->vgid_str, vgid + 6, 38);
		return 1;
	}

	log_warn("WARNING: file data does not begin with a VG name and may be invalid.");

	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt("Write input file data to disk?") == 'n') {
		log_error("Invalid raw text metadata in file.");
		return 0;
	}

	return 1;
}

/* all sizes and offsets in bytes */

static int _read_metadata_file(struct cmd_context *cmd, struct metadata_file *mf)
{
	struct stat sb;
	char *text_buf;
	uint64_t text_size;
	uint32_t text_crc;
	int fd, rv;

	if (!(fd = open(mf->filename, O_RDONLY))) {
		log_error("Cannot open file: %s", mf->filename);
		return 0;
	}

	if (fstat(fd, &sb)) {
		log_error("Cannot access file: %s", mf->filename);
		close(fd);
		return 0;
	}

	if (!(text_size = (uint64_t)sb.st_size)) {
		log_error("Empty file: %s", mf->filename);
		close(fd);
		return 0;
	}

	if (!(text_buf = zalloc(text_size + 1))) {
		close(fd);
		return 0;
	}

	rv = read(fd, text_buf, text_size);
	if (rv != text_size) {
		log_error("Cannot read file: %s", mf->filename);
		close(fd);
		free(text_buf);
		return 0;
	}

	text_size += 1; /* null terminating byte */

	close(fd);

	if (_is_backup_file(cmd, text_buf, text_size)) {
		char *back_buf = text_buf;
		uint64_t back_size = text_size;
		text_buf = NULL;
		text_size = 0;
		if (!_backup_file_to_raw_metadata(back_buf, back_size, &text_buf, &text_size))
			return_0;
	}

	if (!_check_metadata_file(cmd, mf, text_buf, text_size))
		return_0;

	text_crc = calc_crc(INITIAL_CRC, (uint8_t *)text_buf, text_size);

	mf->text_size = text_size;
	mf->text_buf = text_buf;
	mf->text_crc = text_crc;
	return 1;
}

int pvck(struct cmd_context *cmd, int argc, char **argv)
{
	struct settings set;
	struct metadata_file mf;
	struct device *dev;
	const char *dump, *repair;
	const char *pv_name;
	uint64_t labelsector = 1;
	int bad = 0;
	int ret = 0;
	int i;

	memset(&set, 0, sizeof(set));
	memset(&mf, 0, sizeof(mf));

	/*
	 * By default LVM skips the first sector (sector 0), and writes
	 * the label_header in the second sector (sector 1).
	 * (sector size 512 bytes)
	 */
	if (arg_is_set(cmd, labelsector_ARG))
		labelsector = arg_uint64_value(cmd, labelsector_ARG, UINT64_C(0));

	if (arg_is_set(cmd, dump_ARG) || arg_is_set(cmd, repairtype_ARG) || arg_is_set(cmd, repair_ARG)) {
		pv_name = argv[0];

		if (!(dev = dev_cache_get(cmd, pv_name, cmd->filter))) {
			log_error("No device found for %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
			return ECMD_FAILED;
		}
	}

	if (!_get_settings(cmd, &set))
		return ECMD_FAILED;

	if (arg_is_set(cmd, file_ARG) && (arg_is_set(cmd, repairtype_ARG) || arg_is_set(cmd, repair_ARG))) {
		if (!(mf.filename = arg_str_value(cmd, file_ARG, NULL)))
			return ECMD_FAILED;

		if (!_read_metadata_file(cmd, &mf))
			return ECMD_FAILED;
	}

	label_scan_setup_bcache();

	if (arg_is_set(cmd, dump_ARG)) {
		cmd->use_hints = 0;

		dump = arg_str_value(cmd, dump_ARG, NULL);

		if (!strcmp(dump, "metadata"))
			ret = _dump_metadata(cmd, dump, &set, labelsector, dev, PRINT_CURRENT, 0);

		else if (!strcmp(dump, "metadata_all"))
			ret = _dump_metadata(cmd, dump, &set, labelsector, dev, PRINT_ALL, 0);

		else if (!strcmp(dump, "metadata_area"))
			ret = _dump_metadata(cmd, dump, &set, labelsector, dev, 0, 1);

		else if (!strcmp(dump, "metadata_search"))
			ret = _dump_search(cmd, dump, &set, labelsector, dev);

		else if (!strcmp(dump, "headers"))
			ret = _dump_headers(cmd, dump, &set, labelsector, dev);

		else if (!strcmp(dump, "backup_to_raw")) {
			ret = _dump_backup_to_raw(cmd, &set);

		} else
			log_error("Unknown dump value.");

		if (!ret)
			return ECMD_FAILED;
		return ECMD_PROCESSED;
	}

	if (arg_is_set(cmd, repairtype_ARG)) {
		cmd->use_hints = 0;

		repair = arg_str_value(cmd, repairtype_ARG, NULL);

		if (!strcmp(repair, "label_header"))
			ret = _repair_label_header(cmd, repair, &set, labelsector, dev);

		else if (!strcmp(repair, "pv_header"))
			ret = _repair_pv_header(cmd, repair, &set, &mf, labelsector, dev);

		else if (!strcmp(repair, "metadata"))
			ret = _repair_metadata(cmd, repair, &set, &mf, labelsector, dev);
		else
			log_error("Unknown repair value.");

		if (!ret)
			return ECMD_FAILED;
		return ECMD_PROCESSED;
	}

	if (arg_is_set(cmd, repair_ARG)) {
		cmd->use_hints = 0;

		/* repair is a combination of repairtype pv_header+metadata */

		if (!_repair_pv_header(cmd, "pv_header", &set, &mf, labelsector, dev))
			return ECMD_FAILED;

		if (!_repair_metadata(cmd, "metadata", &set, &mf, labelsector, dev))
			return ECMD_FAILED;

		return ECMD_PROCESSED;
	}

	/*
	 * The old/original form of pvck, which did not do much,
	 * but this is here to preserve the historical output.
	 */

	for (i = 0; i < argc; i++) {
		pv_name = argv[i];

		if (!(dev = dev_cache_get(cmd, argv[i], cmd->filter))) {
			log_error("Device %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
			continue;
		}

		if (!_dump_found(cmd, &set, labelsector, dev))
			bad++;
	}

	if (bad)
		return ECMD_FAILED;
	return ECMD_PROCESSED;
}
