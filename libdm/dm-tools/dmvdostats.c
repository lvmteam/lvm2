/*
 * Copyright (C) 2022-2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * VDO statistics output for the dmsetup vdostats command.
 * Verbose (legacy-compatible) and dm_report tabular output.
 * All parsing is done by dm_vdo_stats_parse() in libdm.
 */

#include "libdm/misc/dm-logging.h"
#include "libdm/libdevmapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmvdostats.h"

#define SECTOR_SIZE (1 << DM_SECTOR_SHIFT)
#define SECTORS_PER_KB (1024 / SECTOR_SIZE)

#define MAX_FMT_BUF 80

/* longest derived label is "write amplification ratio" */
#define MAX_DERIVED_LABEL_LEN 25

struct vdo_derived {
	uint64_t physical_size;
	uint64_t logical_size;
	uint64_t used_size;
	int64_t available_size;
	double write_amp;
	int32_t used_pct;
	int32_t saving_pct;
	int emulation_512;
};

struct _verbose_derived {
	uint64_t entries_batching, entries_writing;
	uint64_t blocks_batching, blocks_writing;
};

/*----------------------------------------------------------------
 * Shared helpers
 *----------------------------------------------------------------*/

char *vdo_get_stats(const char *name)
{
	struct dm_task *dmt;
	const char *response;
	char *stats_str = NULL;

	if (!(dmt = dm_task_create(DM_DEVICE_TARGET_MSG)))
		return_NULL;
	if (!dm_task_set_name(dmt, name))
		goto_out;
	if (!dm_task_set_sector(dmt, 0))
		goto_out;
	if (!dm_task_set_message(dmt, "stats"))
		goto_out;
	if (!dm_task_run(dmt))
		goto_out;
	if ((response = dm_task_get_message_response(dmt)))
		stats_str = dm_strdup(response);
out:
	dm_task_destroy(dmt);
	return stats_str;
}

static uint64_t _parse_uint64(const char *str)
{
	char *end;
	uint64_t val;

	val = strtoull(str, &end, 10);
	if (end == str || *end != '\0')
		return 0;

	return val;
}

static void _compute_derived(const struct dm_vdo_stats *vdo,
			     struct vdo_derived *d)
{
	uint64_t sectors_per_block =
		vdo->bytes_per_physical_block ? vdo->bytes_per_physical_block / SECTOR_SIZE
					   : DM_VDO_BLOCK_SIZE;
	uint64_t data_overhead;

	d->physical_size = vdo->physical_blocks * sectors_per_block;
	data_overhead = vdo->data_blocks_used + vdo->overhead_blocks_used;
	d->used_size = data_overhead * sectors_per_block;
	d->logical_size = vdo->logical_blocks * sectors_per_block;

	if (vdo->operating_mode != DM_VDO_MODE_NORMAL) {
		d->available_size = -1;
		d->used_pct = -1;
		d->saving_pct = -1;
	} else {
		if (d->used_size > d->physical_size)
			d->available_size = -1;
		else
			d->available_size = d->physical_size - d->used_size;

		if (vdo->physical_blocks == 0)
			d->used_pct = -1;
		else
			d->used_pct = (int) ((100 * (int64_t) data_overhead +
					      (int64_t) vdo->physical_blocks - 1) /
					     (int64_t) vdo->physical_blocks);

		if (vdo->logical_blocks_used == 0 ||
		    vdo->data_blocks_used > vdo->logical_blocks_used)
			d->saving_pct = -1;
		else
			d->saving_pct = (int) (100 *
					       ((int64_t) vdo->logical_blocks_used -
						(int64_t) vdo->data_blocks_used) /
					       (int64_t) vdo->logical_blocks_used);
	}

	d->write_amp = vdo->bios_in > 0 ?
			       (double) (vdo->bios_out + vdo->bios_meta) /
				       (double) vdo->bios_in :
			       0.0;
	d->emulation_512 = (vdo->bytes_per_logical_block == SECTOR_SIZE) ? 1 : 0;
}

static void _compute_verbose_derived(const struct dm_vdo_stats_full *full,
				     struct _verbose_derived *vd)
{
	uint64_t es = 0, ew = 0, ec = 0;
	uint64_t bs = 0, bw = 0, bc = 0;
	int i;

	for (i = 0; i < full->field_count; i++) {
		const char *l = full->fields[i].label;
		const char *v = full->fields[i].value;

		if (!strcmp(l, "journal entries started"))
			es = _parse_uint64(v);
		else if (!strcmp(l, "journal entries written"))
			ew = _parse_uint64(v);
		else if (!strcmp(l, "journal entries committed"))
			ec = _parse_uint64(v);
		else if (!strcmp(l, "journal blocks started"))
			bs = _parse_uint64(v);
		else if (!strcmp(l, "journal blocks written"))
			bw = _parse_uint64(v);
		else if (!strcmp(l, "journal blocks committed"))
			bc = _parse_uint64(v);
	}

	vd->entries_batching = (es >= ew) ? es - ew : 0;
	vd->entries_writing  = (ew >= ec) ? ew - ec : 0;
	vd->blocks_batching  = (bs >= bw) ? bs - bw : 0;
	vd->blocks_writing   = (bw >= bc) ? bw - bc : 0;
}

/*----------------------------------------------------------------
 * Verbose output
 *----------------------------------------------------------------*/

#define FIXUP_PREFIX      0x01
#define FIXUP_RENAME      0x02
#define FIXUP_NA_RECOVER  0x04
#define FIXUP_NA_ABNORMAL 0x08
#define FIXUP_NA_NORMAL   0x10

static const struct _label_fixup {
	const char *match;
	const char *replace;
	unsigned flags;
} _fixups[] = {
	{ "packer ", "", FIXUP_PREFIX },
	{ "allocator ", "", FIXUP_PREFIX },
	{ "hash lock ", "", FIXUP_PREFIX },
	{ "errors ", "", FIXUP_PREFIX },
	{ "index ", "", FIXUP_PREFIX },
	{ "memory usage ", "", FIXUP_PREFIX },
	{ "complete recoveries", "completed recovery count", FIXUP_RENAME },
	{ "read only recoveries", "read-only recovery count", FIXUP_RENAME },
	{ "mode", "operating mode", FIXUP_RENAME },
	{ "recovery percentage", "recovery progress (%)", FIXUP_RENAME | FIXUP_NA_NORMAL },
	{ "journal disk full", "journal disk full count", FIXUP_RENAME },
	{ "journal slab journal commits requested", "journal commits requested count",
	  FIXUP_RENAME },
	{ "ref counts blocks written", "reference blocks written", FIXUP_RENAME },
	{ "current VIOs in progress", "current VDO IO requests in progress",
	  FIXUP_RENAME },
	{ "max VIOs", "maximum VDO IO requests in progress", FIXUP_RENAME },
	{ "bytes used", "KVDO module bytes used", FIXUP_RENAME },
	{ "peak bytes used", "KVDO module peak bytes used", FIXUP_RENAME },
	{ "curr dedupe queries", "current dedupe queries", FIXUP_RENAME },
	{ "in recovery mode", "", FIXUP_RENAME },
	{ "logical block size", "", FIXUP_RENAME },
	{ "overhead blocks used", "", FIXUP_NA_RECOVER },
	{ "data blocks used", "", FIXUP_NA_ABNORMAL },
	{ "logical blocks used", "", FIXUP_NA_RECOVER },
};

static void _fixup_labels(struct dm_vdo_stats_full *full)
{
	int i, j;
	int recovering, readonly;
	unsigned na_mask;
	size_t mlen;

	recovering = (full->stats->operating_mode == DM_VDO_MODE_RECOVERING);
	readonly = (full->stats->operating_mode == DM_VDO_MODE_READ_ONLY);

	na_mask = 0;
	if (recovering)
		na_mask |= FIXUP_NA_RECOVER | FIXUP_NA_ABNORMAL;
	if (readonly)
		na_mask |= FIXUP_NA_ABNORMAL;
	if (!recovering)
		na_mask |= FIXUP_NA_NORMAL;

	for (i = 0; i < full->field_count; i++) {
		struct dm_vdo_stats_field *fld = &full->fields[i];

		for (j = 0; j < (int) DM_ARRAY_SIZE(_fixups); j++) {
			mlen = strlen(_fixups[j].match);

			if (_fixups[j].flags & FIXUP_PREFIX) {
				if (!strncmp(fld->label,
					     _fixups[j].match,
					     mlen)) {
					char tmp[sizeof(fld->label)];
					snprintf(tmp,
						 sizeof(tmp),
						 "%s%s",
						 _fixups[j].replace,
						 fld->label + mlen);
					snprintf(fld->label,
						 sizeof(fld->label),
						 "%s",
						 tmp);
				}
				continue;
			}

			if (!strcmp(fld->label, _fixups[j].match)) {
				if (_fixups[j].flags & FIXUP_RENAME)
					snprintf(fld->label,
						 sizeof(fld->label),
						 "%s",
						 _fixups[j].replace);

				if (_fixups[j].flags & na_mask)
					snprintf(fld->value,
						 sizeof(fld->value),
						 "N/A");
				break;
			}
		}
	}
}

static void _print_field(const char *label, const char *value, int max_len)
{
	printf("  %s%*s : %s\n",
	       label, max_len - (int) strlen(label), "", value);
}

static void _print_field_na(const char *label, const char *value,
			    int na, int max_len)
{
	_print_field(label, na ? "N/A" : value, max_len);
}

static void _print_size_fields(const struct vdo_derived *d,
			       int na, int max_len)
{
	char buf[MAX_FMT_BUF];

	snprintf(buf, sizeof(buf), "%" PRIu64, d->physical_size / SECTORS_PER_KB);
	_print_field("1K-blocks", buf, max_len);

	snprintf(buf, sizeof(buf), "%" PRIu64, d->used_size / SECTORS_PER_KB);
	_print_field_na("1K-blocks used", buf, na, max_len);

	snprintf(buf, sizeof(buf), "%" PRId64, d->available_size / SECTORS_PER_KB);
	_print_field_na("1K-blocks available", buf,
			na || d->available_size < 0, max_len);

	snprintf(buf, sizeof(buf), "%d", d->used_pct);
	_print_field_na("used percent", buf, na || d->used_pct < 0, max_len);

	snprintf(buf, sizeof(buf), "%d", d->saving_pct);
	_print_field_na("saving percent", buf, na || d->saving_pct < 0, max_len);
}

static int _max_label_length(const struct dm_vdo_stats_full *full)
{
	int i, len, max = MAX_DERIVED_LABEL_LEN;

	for (i = 0; i < full->field_count; i++) {
		if (!full->fields[i].label[0])
			continue;
		len = (int) strlen(full->fields[i].label);
		if (len > max)
			max = len;
	}

	return max;
}

static void _print_verbose(const char *name,
			   const struct dm_vdo_stats_full *full,
			   const struct vdo_derived *d,
			   const struct _verbose_derived *vd)
{
	int i, max_len, na;
	char buf[MAX_FMT_BUF];
	const char *l;

	na = (full->stats->operating_mode != DM_VDO_MODE_NORMAL);
	max_len = _max_label_length(full);

	printf("%s :\n", name ? name : "VDO");

	for (i = 0; i < full->field_count; i++) {
		l = full->fields[i].label;

		if (!l[0])
			continue;

		if (!strcmp(l, "journal entries started")) {
			snprintf(buf, sizeof(buf), "%" PRIu64,
				 vd->entries_batching);
			_print_field("journal entries batching", buf, max_len);
		} else if (!strcmp(l, "journal entries written")) {
			snprintf(buf, sizeof(buf), "%" PRIu64,
				 vd->entries_writing);
			_print_field("journal entries writing", buf, max_len);
		} else if (!strcmp(l, "journal blocks started")) {
			snprintf(buf, sizeof(buf), "%" PRIu64,
				 vd->blocks_batching);
			_print_field("journal blocks batching", buf, max_len);
		} else if (!strcmp(l, "journal blocks written")) {
			snprintf(buf, sizeof(buf), "%" PRIu64,
				 vd->blocks_writing);
			_print_field("journal blocks writing", buf, max_len);
		}

		_print_field(l, full->fields[i].value, max_len);

		if (!strcmp(l, "logical blocks"))
			_print_size_fields(d, na, max_len);
		else if (!strcmp(l, "flush out")) {
			snprintf(buf, sizeof(buf), "%.2f", d->write_amp);
			_print_field("write amplification ratio",
				     buf, max_len);
		} else if (!strcmp(l, "instance")) {
			_print_field("512 byte emulation",
				     d->emulation_512 ? "on" : "off",
				     max_len);
		}
	}
}

int vdostats_print_verbose(const char *name, const char *stats_str)
{
	struct dm_vdo_stats_full *full;
	struct vdo_derived d;
	struct _verbose_derived vd;

	if (!stats_str)
		return 0;

	full = dm_vdo_stats_parse(NULL, stats_str, DM_VDO_STATS_FULL);
	if (!full)
		return 0;

	_fixup_labels(full);
	_compute_derived(full->stats, &d);
	_compute_verbose_derived(full, &vd);
	_print_verbose(name, full, &d, &vd);

	dm_free(full);
	return 1;
}

/*----------------------------------------------------------------
 * Report (tabular) output
 *----------------------------------------------------------------*/

struct vdo_report_obj {
	const char *name;
	struct dm_vdo_stats *vdo;
	struct vdo_derived derived;
};

#define DR_VDO 0x1

static const char _vdo_default_options[] =
	"vdo_name,vdo_physical_size,vdo_used,vdo_available,vdo_used_pct,vdo_saving_pct";

static void *_vdo_report_obj(void *obj)
{
	return obj;
}

static const struct dm_report_object_type _vdo_report_types[] = {
	{ DR_VDO, "VDO", "vdo_", _vdo_report_obj },
	{ 0, "", "", NULL }
};

#define FIELD_F(id, header, width, func, help)     \
	{                                          \
		DR_VDO, DM_REPORT_FIELD_TYPE_SIZE, \
		0,	width,                     \
		#id,	header,                    \
		&func,	help                       \
	}
#define FIELD_S(id, header, width, func, help)                               \
	{ DR_VDO, DM_REPORT_FIELD_TYPE_STRING, 0, width, #id, header, &func, \
	  help }

static int _vdo_name_disp(struct dm_report *rh,
			  struct dm_pool *mem __attribute__((unused)),
			  struct dm_report_field *field,
			  const void *data,
			  void *private __attribute__((unused)))
{
	const struct vdo_report_obj *obj =
		(const struct vdo_report_obj *) data;
	const char *name = obj->name ? obj->name : "";

	return dm_report_field_string(rh, field, &name);
}

static int _vdo_size_disp(struct dm_report *rh,
			  struct dm_pool *mem,
			  struct dm_report_field *field,
			  const void *data,
			  uint64_t sectors,
			  int show_na)
{
	const struct vdo_report_obj *obj =
		(const struct vdo_report_obj *) data;
	const char *repstr;
	const char *na_str = "N/A";

	if (show_na && obj->vdo->operating_mode != DM_VDO_MODE_NORMAL)
		return dm_report_field_string(rh, field, &na_str);

	repstr = dm_size_to_string(mem,
				   sectors,
				   get_disp_units(),
				   1,
				   get_disp_factor(),
				   show_units(),
				   DM_SIZE_UNIT);
	if (!repstr)
		return 0;

	return dm_report_field_string(rh, field, &repstr);
}

static int _vdo_phys_size_disp(struct dm_report *rh,
			       struct dm_pool *mem,
			       struct dm_report_field *field,
			       const void *data,
			       void *private __attribute__((unused)))
{
	const struct vdo_report_obj *obj =
		(const struct vdo_report_obj *) data;
	return _vdo_size_disp(
		rh, mem, field, data, obj->derived.physical_size, 0);
}

static int _vdo_used_disp(struct dm_report *rh,
			  struct dm_pool *mem,
			  struct dm_report_field *field,
			  const void *data,
			  void *private __attribute__((unused)))
{
	const struct vdo_report_obj *obj =
		(const struct vdo_report_obj *) data;
	return _vdo_size_disp(rh, mem, field, data, obj->derived.used_size, 1);
}

static int _vdo_avail_disp(struct dm_report *rh,
			   struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data,
			   void *private __attribute__((unused)))
{
	const struct vdo_report_obj *obj =
		(const struct vdo_report_obj *) data;
	const char *na_str = "N/A";

	if (obj->derived.available_size < 0)
		return dm_report_field_string(rh, field, &na_str);

	return _vdo_size_disp(rh,
			      mem,
			      field,
			      data,
			      (uint64_t) obj->derived.available_size,
			      1);
}

static int
_vdo_pct_disp(struct dm_report *rh, struct dm_report_field *field, int32_t pct)
{
	char buf[16];
	const char *s;

	if (pct < 0) {
		s = "N/A";
		return dm_report_field_string(rh, field, &s);
	}
	snprintf(buf, sizeof(buf), "%d%%", pct);
	s = buf;
	return dm_report_field_string(rh, field, &s);
}

static int _vdo_used_pct_disp(struct dm_report *rh,
			      struct dm_pool *mem __attribute__((unused)),
			      struct dm_report_field *field,
			      const void *data,
			      void *private __attribute__((unused)))
{
	const struct vdo_report_obj *obj =
		(const struct vdo_report_obj *) data;
	return _vdo_pct_disp(rh, field, obj->derived.used_pct);
}

static int _vdo_saving_pct_disp(struct dm_report *rh,
				struct dm_pool *mem __attribute__((unused)),
				struct dm_report_field *field,
				const void *data,
				void *private __attribute__((unused)))
{
	const struct vdo_report_obj *obj =
		(const struct vdo_report_obj *) data;
	return _vdo_pct_disp(rh, field, obj->derived.saving_pct);
}

static const struct dm_report_field_type _vdo_report_fields[] = {
	FIELD_S(vdo_name, "Device", 6, _vdo_name_disp, "VDO device name."),
	FIELD_F(vdo_physical_size,
		"Size",
		9,
		_vdo_phys_size_disp,
		"Physical size of VDO device."),
	FIELD_F(vdo_used,
		"Used",
		9,
		_vdo_used_disp,
		"Used size of VDO device."),
	FIELD_F(vdo_available,
		"Available",
		9,
		_vdo_avail_disp,
		"Available size of VDO device."),
	FIELD_S(vdo_used_pct,
		"Use%",
		4,
		_vdo_used_pct_disp,
		"Percentage of physical space used."),
	FIELD_S(vdo_saving_pct,
		"Space saving%",
		14,
		_vdo_saving_pct_disp,
		"Space saving percentage."),
	{ 0, 0, 0, 0, "", "", NULL, NULL }
};

#undef FIELD_F
#undef FIELD_S

struct dm_report *vdostats_report_init(const char *output_fields,
				       const char *output_separator,
				       uint32_t output_flags,
				       const char *sort_keys,
				       const char *selection)
{
	uint32_t report_type = DR_VDO;

	if (!output_fields || !output_fields[0])
		output_fields = _vdo_default_options;

	return dm_report_init_with_selection(&report_type,
					     _vdo_report_types,
					     _vdo_report_fields,
					     output_fields,
					     output_separator,
					     output_flags,
					     sort_keys,
					     selection,
					     NULL,
					     NULL);
}

int vdostats_report_device(struct dm_report *rh,
			   const char *name,
			   const char *stats_str)
{
	struct vdo_report_obj obj;
	struct dm_vdo_stats_full *full;
	int r;

	full = dm_vdo_stats_parse(NULL, stats_str, DM_VDO_STATS_BASIC);
	if (!full)
		return 0;

	obj.vdo = full->stats;
	obj.name = name;
	_compute_derived(obj.vdo, &obj.derived);

	r = dm_report_object(rh, &obj);

	dm_free(full);
	return r;
}
