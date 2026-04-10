/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "units.h"
#include "lib/format_text/layout.h"
#include "lib/label/label.h"
#include "lib/misc/crc.h"
#include "lib/mm/xlate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Metadata parsing security regression tests.
 *
 * Build CRC-valid on-disk structures with crafted attack payloads
 * and verify that validation checks correctly reject them.
 */

/*----------------------------------------------------------------
 * Helpers to build CRC-valid on-disk structures
 *--------------------------------------------------------------*/

/* Set label header fields and compute CRC (bytes 20-511). */
static void _label_set_crc(char *buf)
{
	struct label_header *lh = (struct label_header *)buf;

	lh->crc_xl = htole32(calc_crc(INITIAL_CRC,
		(const uint8_t *)&lh->offset_xl,
		LABEL_SIZE - offsetof(struct label_header, offset_xl)));
}

/* Build a 512-byte label sector. Payload area is zeroed. */
static void _build_label(char *buf, uint32_t offset_xl)
{
	struct label_header *lh = (struct label_header *)buf;

	memset(buf, 0, LABEL_SIZE);
	memcpy(lh->id, LABEL_ID, 8);
	lh->sector_xl = htole64(1);
	lh->offset_xl = htole32(offset_xl);
	memcpy(lh->type, LVM2_LABEL, 8);
	_label_set_crc(buf);
}

/* Build a 512-byte MDA header with one raw_locn + null terminator. */
static void _build_mda_header(char *buf, uint64_t start, uint64_t mda_size,
			      uint64_t rlocn_offset, uint64_t rlocn_size)
{
	struct mda_header *mdah = (struct mda_header *)buf;

	memset(buf, 0, MDA_HEADER_SIZE);
	memcpy(mdah->magic, FMTT_MAGIC, 16);
	mdah->version = htole32(FMTT_VERSION);
	mdah->start = htole64(start);
	mdah->size = htole64(mda_size);

	mdah->raw_locns[0].offset = htole64(rlocn_offset);
	mdah->raw_locns[0].size = htole64(rlocn_size);
	mdah->raw_locns[0].checksum = 0;
	mdah->raw_locns[0].flags = 0;
	/* raw_locns[1] is already zeroed (null terminator) */

	/* CRC covers magic through end (bytes 4-511) */
	mdah->checksum_xl = htole32(calc_crc(INITIAL_CRC,
		(const uint8_t *)mdah->magic,
		MDA_HEADER_SIZE - sizeof(mdah->checksum_xl)));
}

/*----------------------------------------------------------------
 * PV header offset validation
 *
 * Craft: label sector with offset_xl pointing past end of sector.
 * Without validation, pvhdr points into out-of-bounds memory.
 *--------------------------------------------------------------*/

static void test_label_offset_oob(void *fixture)
{
	char label_buf[LABEL_SIZE];
	struct label_header *lh;

	/* offset_xl = 0x2000 (8192), well past end of sector */
	_build_label(label_buf, 0x2000);
	lh = (struct label_header *)label_buf;

	/* Verify CRC is valid (well-formed attack payload) */
	T_ASSERT_EQUAL(lh->crc_xl,
		htole32(calc_crc(INITIAL_CRC,
			(const uint8_t *)&lh->offset_xl,
			LABEL_SIZE - offsetof(struct label_header, offset_xl))));

	/* Must reject OOB offset */
	T_ASSERT(!label_check_pv_layout(label_buf, NULL, NULL, NULL));
}

static void test_label_offset_one_past(void *fixture)
{
	char label_buf[LABEL_SIZE];

	_build_label(label_buf, LABEL_SIZE - sizeof(struct pv_header) + 1);

	/* One byte past valid range */
	T_ASSERT(!label_check_pv_layout(label_buf, NULL, NULL, NULL));
}

static void test_label_offset_valid(void *fixture)
{
	char label_buf[LABEL_SIZE];

	/* Normal offset: sizeof(struct label_header) = 32 */
	_build_label(label_buf, sizeof(struct label_header));

	T_ASSERT(label_check_pv_layout(label_buf, NULL, NULL, NULL));
}

/*----------------------------------------------------------------
 * disk_locn bounds checking
 *
 * Craft: label with PV header and disk_locn entries filling the
 * entire 512-byte sector (no null terminator).
 * Without bounds checking the while loop walks past the end of
 * the label buffer.
 *--------------------------------------------------------------*/

static void test_disk_locn_no_terminator(void *fixture)
{
	char label_buf[LABEL_SIZE];
	struct pv_header *pvhdr;
	struct disk_locn *dlocn_xl;
	struct disk_locn *label_end;
	int count;

	/* Build label with normal offset, fill pv_header fields */
	_build_label(label_buf, sizeof(struct label_header));
	pvhdr = (struct pv_header *)(label_buf + sizeof(struct label_header));
	memset(pvhdr->pv_uuid, 'A', ID_LEN);
	pvhdr->device_size_xl = htole64(0x100000000ULL);

	/* Fill all disk_locn slots with non-zero entries:
	 * no null terminator forces the loop to rely on bounds checking. */
	label_end = (struct disk_locn *)(label_buf + LABEL_SIZE);
	for (dlocn_xl = pvhdr->disk_areas_xl, count = 0;
	     (dlocn_xl + 1) <= label_end;
	     dlocn_xl++, count++) {
		dlocn_xl->offset = htole64(0x100000 + count * 0x1000ULL);
		dlocn_xl->size = htole64(0x1000);
	}
	T_ASSERT(count > 0);

	/* Recompute CRC after filling payload */
	_label_set_crc(label_buf);

	/* Must reject: no null terminator within bounds */
	T_ASSERT(!label_check_pv_layout(label_buf, NULL, NULL, NULL));
}

static void test_disk_locn_extension_overflow(void *fixture)
{
	char label_buf[LABEL_SIZE];
	struct pv_header *pvhdr;
	struct pv_header_extension *ext = NULL;
	struct disk_locn *dlocn_xl;
	struct disk_locn *label_end;

	/*
	 * Use offset_xl=40 so disk_areas_xl starts at byte 80 (16-byte
	 * aligned), causing the last metadata null terminator to end at
	 * exactly LABEL_SIZE with no room for pv_header_extension.
	 */
	_build_label(label_buf, 40);
	pvhdr = (struct pv_header *)(label_buf + 40);
	memset(pvhdr->pv_uuid, 'A', ID_LEN);
	pvhdr->device_size_xl = htole64(0x100000000ULL);
	label_end = (struct disk_locn *)(label_buf + LABEL_SIZE);

	/* Data areas: one entry + null terminator */
	dlocn_xl = pvhdr->disk_areas_xl;
	dlocn_xl->offset = htole64(0x100000);
	dlocn_xl->size = htole64(0x1000);
	dlocn_xl++;
	dlocn_xl->offset = 0;
	dlocn_xl->size = 0;
	dlocn_xl++;

	/* Fill metadata entries to consume remaining space */
	while ((dlocn_xl + 2) <= label_end) {
		dlocn_xl->offset = htole64(0x1000);
		dlocn_xl->size = htole64(0x100000);
		dlocn_xl++;
	}
	dlocn_xl->offset = 0;
	dlocn_xl->size = 0;
	_label_set_crc(label_buf);

	/* Layout is valid but extension doesn't fit */
	T_ASSERT(label_check_pv_layout(label_buf, NULL, NULL, &ext));
	T_ASSERT(!ext);
}

static void test_disk_locn_normal(void *fixture)
{
	char label_buf[LABEL_SIZE];
	struct pv_header *pvhdr;
	struct pv_header_extension *ext = NULL;
	struct disk_locn *dlocn_xl;

	_build_label(label_buf, sizeof(struct label_header));
	pvhdr = (struct pv_header *)(label_buf + sizeof(struct label_header));
	memset(pvhdr->pv_uuid, 'A', ID_LEN);
	pvhdr->device_size_xl = htole64(0x100000000ULL);

	/* One data area + null terminator */
	dlocn_xl = pvhdr->disk_areas_xl;
	dlocn_xl->offset = htole64(0x100000);
	dlocn_xl->size = htole64(0x1000);
	dlocn_xl++;
	dlocn_xl->offset = 0;
	dlocn_xl->size = 0;

	_label_set_crc(label_buf);

	/* Normal case: valid layout with room for extension */
	T_ASSERT(label_check_pv_layout(label_buf, NULL, NULL, &ext));
	T_ASSERT(ext);
}

/*----------------------------------------------------------------
 * MDA header size validation
 *
 * mdah->size = 0 causes underflow in (mdah->size - MDA_HEADER_SIZE).
 *--------------------------------------------------------------*/

static void test_mda_size_zero(void *fixture)
{
	char mda_buf[MDA_HEADER_SIZE];
	struct device_area dev_area = { .dev = NULL, .start = 4096 };
	uint32_t bad_fields = 0;
	int ret;

	/* mdah->size = 0, valid CRC */
	_build_mda_header(mda_buf, 4096, 0, MDA_HEADER_SIZE, 4096);

	ret = raw_parse_mda_header((struct mda_header *)mda_buf,
				   &dev_area, 0, &bad_fields);

	/* Parser must reject: size < MDA_HEADER_SIZE */
	T_ASSERT(!ret);
	T_ASSERT(bad_fields & BAD_MDA_HEADER);
}

static void test_mda_size_one(void *fixture)
{
	char mda_buf[MDA_HEADER_SIZE];
	struct device_area dev_area = { .dev = NULL, .start = 4096 };
	uint32_t bad_fields = 0;
	int ret;

	_build_mda_header(mda_buf, 4096, 1, MDA_HEADER_SIZE, 4096);

	ret = raw_parse_mda_header((struct mda_header *)mda_buf,
				   &dev_area, 0, &bad_fields);

	T_ASSERT(!ret);
	T_ASSERT(bad_fields & BAD_MDA_HEADER);
}

static void test_mda_size_valid(void *fixture)
{
	char mda_buf[MDA_HEADER_SIZE];
	struct device_area dev_area = { .dev = NULL, .start = 4096 };
	uint32_t bad_fields = 0;
	int ret;

	/* Normal: 1MB metadata area */
	_build_mda_header(mda_buf, 4096, 1024 * 1024, MDA_HEADER_SIZE, 4096);

	ret = raw_parse_mda_header((struct mda_header *)mda_buf,
				   &dev_area, 0, &bad_fields);

	/* Valid header, no bad fields */
	T_ASSERT(ret);
	T_ASSERT(!bad_fields);
}

/*----------------------------------------------------------------
 * rlocn bounds checking
 *
 * rlocn->size > UINT32_MAX causes truncation in wrap calculation.
 * rlocn->offset >= mdah->size allows out-of-bounds read.
 *--------------------------------------------------------------*/

static void test_rlocn_size_truncation(void *fixture)
{
	char mda_buf[MDA_HEADER_SIZE];
	struct mda_header *mdah;
	struct raw_locn *rlocn;
	struct device_area dev_area = { .dev = NULL, .start = 4096 };
	uint32_t bad_fields = 0;

	/* Values that pass first two bounds but fail UINT32_MAX check */
	uint64_t mdah_size = 0x200000000ULL;     /* 8 GB */
	uint64_t rlocn_offset = 0x100000000ULL;  /* 4 GB */
	uint64_t rlocn_size = 0x1FFFFFE00ULL;    /* ~8 GB */

	_build_mda_header(mda_buf, 4096, mdah_size, rlocn_offset, rlocn_size);

	/* Header itself is valid (size >= MDA_HEADER_SIZE) */
	T_ASSERT(raw_parse_mda_header((struct mda_header *)mda_buf,
				      &dev_area, 0, &bad_fields));
	T_ASSERT(!bad_fields);

	mdah = (struct mda_header *)mda_buf;
	rlocn = &mdah->raw_locns[0];

	/* First two bounds pass (the trap): */
	T_ASSERT(rlocn->offset < mdah->size);
	T_ASSERT(rlocn->size <= mdah->size - MDA_HEADER_SIZE);

	/* Would truncate to uint32_t in wrap calculation */
	T_ASSERT(rlocn->size > UINT32_MAX);
}

static void test_rlocn_offset_exceeds_mda(void *fixture)
{
	char mda_buf[MDA_HEADER_SIZE];
	struct mda_header *mdah;
	struct raw_locn *rlocn;
	struct device_area dev_area = { .dev = NULL, .start = 4096 };
	uint32_t bad_fields = 0;

	/* rlocn->offset >= mdah->size */
	_build_mda_header(mda_buf, 4096, 1024 * 1024,
			  2 * 1024 * 1024, 4096);

	T_ASSERT(raw_parse_mda_header((struct mda_header *)mda_buf,
				      &dev_area, 0, &bad_fields));

	mdah = (struct mda_header *)mda_buf;
	rlocn = &mdah->raw_locns[0];

	/* Offset past end of metadata area */
	T_ASSERT(rlocn->offset >= mdah->size);
}

static void test_rlocn_valid(void *fixture)
{
	char mda_buf[MDA_HEADER_SIZE];
	struct mda_header *mdah;
	struct raw_locn *rlocn;
	struct device_area dev_area = { .dev = NULL, .start = 4096 };
	uint32_t bad_fields = 0;

	/* Normal: 1MB MDA, offset=512, size=4096 */
	_build_mda_header(mda_buf, 4096, 1024 * 1024, MDA_HEADER_SIZE, 4096);

	T_ASSERT(raw_parse_mda_header((struct mda_header *)mda_buf,
				      &dev_area, 0, &bad_fields));
	T_ASSERT(!bad_fields);

	mdah = (struct mda_header *)mda_buf;
	rlocn = &mdah->raw_locns[0];

	T_ASSERT(rlocn->offset < mdah->size);
	T_ASSERT(rlocn->size <= mdah->size - MDA_HEADER_SIZE);
	T_ASSERT(rlocn->size <= UINT32_MAX);
}

/*----------------------------------------------------------------
 * Test suite registration
 *--------------------------------------------------------------*/

#define T(path, desc, fn) register_test(ts, "/base/metadata/security/" path, desc, fn)

void metadata_security_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	/* PV header offset bounds */
	T("label-offset/oob", "label offset_xl 0x2000 OOB", test_label_offset_oob);
	T("label-offset/one-past", "label offset one past boundary", test_label_offset_one_past);
	T("label-offset/valid", "label offset normal", test_label_offset_valid);

	/* disk_locn iteration bounds */
	T("disk-locn/no-terminator", "disk_locn array no null terminator", test_disk_locn_no_terminator);
	T("disk-locn/extension-overflow", "extension exceeds label", test_disk_locn_extension_overflow);
	T("disk-locn/normal", "disk_locn normal layout", test_disk_locn_normal);

	/* MDA header size validation */
	T("mda-size/zero", "mdah->size zero underflow", test_mda_size_zero);
	T("mda-size/one", "mdah->size one byte", test_mda_size_one);
	T("mda-size/valid", "mdah->size normal", test_mda_size_valid);

	/* rlocn size truncation */
	T("rlocn/size-truncation", "rlocn->size > UINT32_MAX", test_rlocn_size_truncation);
	T("rlocn/offset-oob", "rlocn->offset >= mdah->size", test_rlocn_offset_exceeds_mda);
	T("rlocn/valid", "rlocn normal values", test_rlocn_valid);

	dm_list_add(all_tests, &ts->list);
}
