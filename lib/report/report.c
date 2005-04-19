/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "metadata.h"
#include "report.h"
#include "toolcontext.h"
#include "pool.h"
#include "lvm-string.h"
#include "display.h"
#include "activate.h"
#include "segtype.h"

/* 
 * For macro use
 */
static union {
	struct physical_volume _pv;
	struct logical_volume _lv;
	struct volume_group _vg;
	struct lv_segment _seg;
	struct pv_segment _pvseg;
} _dummy;

/*
 * Report handle flags
 */
#define RH_SORT_REQUIRED	0x00000001
#define RH_HEADINGS_PRINTED	0x00000002
#define RH_BUFFERED		0x00000004
#define RH_ALIGNED		0x00000008
#define RH_HEADINGS		0x00000010

struct report_handle {
	struct cmd_context *cmd;
	struct pool *mem;

	report_type_t type;
	const char *field_prefix;
	uint32_t flags;
	const char *separator;

	uint32_t keys_count;

	/* Ordered list of fields needed for this report */
	struct list field_props;

	/* Rows of report data */
	struct list rows;
};

/* 
 * Per-field flags
 */
#define FLD_ALIGN_LEFT	0x00000001
#define FLD_ALIGN_RIGHT	0x00000002
#define FLD_STRING	0x00000004
#define FLD_NUMBER	0x00000008
#define FLD_HIDDEN	0x00000010
#define FLD_SORT_KEY	0x00000020
#define FLD_ASCENDING	0x00000040
#define FLD_DESCENDING	0x00000080

struct field_properties {
	struct list list;
	uint32_t field_num;
	uint32_t sort_posn;
	int width;
	uint32_t flags;
};

/* 
 * Report data
 */
struct field {
	struct list list;
	struct field_properties *props;

	const char *report_string;	/* Formatted ready for display */
	const void *sort_value;	/* Raw value for sorting */
};

struct row {
	struct list list;
	struct report_handle *rh;
	struct list fields;	/* Fields in display order */
	struct field *(*sort_fields)[];	/* Fields in sort order */
};

static char _alloc_policy_char(alloc_policy_t alloc)
{
	switch (alloc) {
	case ALLOC_CONTIGUOUS:
		return 'c';
	case ALLOC_NORMAL:
		return 'n';
	case ALLOC_ANYWHERE:
		return 'a';
	default:
		return 'i';
	}
}

/*
 * Data-munging functions to prepare each data type for display and sorting
 */
static int _string_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	if (!
	    (field->report_string =
	     pool_strdup(rh->mem, *(const char **) data))) {
		log_error("pool_strdup failed");
		return 0;
	}

	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _dev_name_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	const char *name = dev_name(*(const struct device **) data);

	return _string_disp(rh, field, &name);
}

static int _devices_disp(struct report_handle *rh, struct field *field,
			 const void *data)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	unsigned int s;
	const char *name;
	uint32_t extent;
	char extent_str[32];

	if (!pool_begin_object(rh->mem, 256)) {
		log_error("pool_begin_object failed");
		return 0;
	}

	for (s = 0; s < seg->area_count; s++) {
		switch (seg->area[s].type) {
		case AREA_LV:
			name = seg->area[s].u.lv.lv->name;
			extent = seg->area[s].u.lv.le;
			break;
		case AREA_PV:
			name = dev_name(seg->area[s].u.pv.pv->dev);
			extent = seg->area[s].u.pv.pe;
			break;
		default:
			name = "unknown";
			extent = 0;
		}

		if (!pool_grow_object(rh->mem, name, strlen(name))) {
			log_error("pool_grow_object failed");
			return 0;
		}

		if (lvm_snprintf(extent_str, sizeof(extent_str), "(%" PRIu32
				 ")", extent) < 0) {
			log_error("Extent number lvm_snprintf failed");
			return 0;
		}

		if (!pool_grow_object(rh->mem, extent_str, strlen(extent_str))) {
			log_error("pool_grow_object failed");
			return 0;
		}

		if ((s != seg->area_count - 1) &&
		    !pool_grow_object(rh->mem, ",", 1)) {
			log_error("pool_grow_object failed");
			return 0;
		}
	}

	if (!pool_grow_object(rh->mem, "\0", 1)) {
		log_error("pool_grow_object failed");
		return 0;
	}

	field->report_string = pool_end_object(rh->mem);
	field->sort_value = (const void *) field->report_string;

	return 1;
}
static int _tags_disp(struct report_handle *rh, struct field *field,
		      const void *data)
{
	const struct list *tags = (const struct list *) data;
	struct str_list *sl;

	if (!pool_begin_object(rh->mem, 256)) {
		log_error("pool_begin_object failed");
		return 0;
	}

	list_iterate_items(sl, tags) {
		if (!pool_grow_object(rh->mem, sl->str, strlen(sl->str)) ||
		    (sl->list.n != tags && !pool_grow_object(rh->mem, ",", 1))) {
			log_error("pool_grow_object failed");
			return 0;
		}
	}

	if (!pool_grow_object(rh->mem, "\0", 1)) {
		log_error("pool_grow_object failed");
		return 0;
	}

	field->report_string = pool_end_object(rh->mem);
	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _vgfmt_disp(struct report_handle *rh, struct field *field,
		       const void *data)
{
	const struct volume_group *vg = (const struct volume_group *) data;

	if (!vg->fid) {
		field->report_string = "";
		field->sort_value = (const void *) field->report_string;
		return 1;
	}

	return _string_disp(rh, field, &vg->fid->fmt->name);
}

static int _pvfmt_disp(struct report_handle *rh, struct field *field,
		       const void *data)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;

	if (!pv->fmt) {
		field->report_string = "";
		field->sort_value = (const void *) field->report_string;
		return 1;
	}

	return _string_disp(rh, field, &pv->fmt->name);
}

static int _int_disp(struct report_handle *rh, struct field *field,
		     const void *data)
{
	const int value = *(const int *) data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = pool_zalloc(rh->mem, 13))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!(sortval = pool_alloc(rh->mem, sizeof(int64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (lvm_snprintf(repstr, 12, "%d", value) < 0) {
		log_error("int too big: %d", value);
		return 0;
	}

	*sortval = (const uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

static int _lvkmaj_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	struct lvinfo info;
	uint64_t minusone = UINT64_C(-1);

	if (lv_info(lv, &info, 0) && info.exists)
		return _int_disp(rh, field, &info.major);
	else
		return _int_disp(rh, field, &minusone);

	return 1;
}

static int _lvkmin_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	struct lvinfo info;
	uint64_t minusone = UINT64_C(-1);

	if (lv_info(lv, &info, 0) && info.exists)
		return _int_disp(rh, field, &info.minor);
	else
		return _int_disp(rh, field, &minusone);

	return 1;
}

static int _lvstatus_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	struct lvinfo info;
	char *repstr;
	struct lv_segment *snap_seg;
	float snap_percent;

	if (!(repstr = pool_zalloc(rh->mem, 7))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (lv->status & PVMOVE)
		repstr[0] = 'p';
	else if (lv->status & MIRRORED)
		repstr[0] = 'm';
	else if (lv->status & VIRTUAL)
		repstr[0] = 'v';
	else if (lv_is_origin(lv))
		repstr[0] = 'o';
	else if (find_cow(lv))
		repstr[0] = 's';
	else
		repstr[0] = '-';

	if (lv->status & PVMOVE)
		repstr[1] = '-';
	else if (lv->status & LVM_WRITE)
		repstr[1] = 'w';
	else
		repstr[1] = 'r';

	repstr[2] = _alloc_policy_char(lv->alloc);

	if (lv->status & LOCKED)
		repstr[2] = toupper(repstr[2]);

	if (lv->status & FIXED_MINOR)
		repstr[3] = 'm';	/* Fixed Minor */
	else
		repstr[3] = '-';

	if (lv_info(lv, &info, 1) && info.exists) {
		if (info.suspended)
			repstr[4] = 's';	/* Suspended */
		else
			repstr[4] = 'a';	/* Active */
		if (info.open_count)
			repstr[5] = 'o';	/* Open */
		else
			repstr[5] = '-';

		/* Snapshot dropped? */
		if ((snap_seg = find_cow(lv)) &&
		    (!lv_snapshot_percent(snap_seg->cow, &snap_percent) ||
		     snap_percent < 0 || snap_percent >= 100)) {
			repstr[0] = toupper(repstr[0]);
			if (info.suspended)
				repstr[4] = 'S';
			else
				repstr[4] = 'I';
		}

	} else {
		repstr[4] = '-';
		repstr[5] = '-';
	}

	field->report_string = repstr;
	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _pvstatus_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	const uint32_t status = *(const uint32_t *) data;
	char *repstr;

	if (!(repstr = pool_zalloc(rh->mem, 4))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (status & ALLOCATABLE_PV)
		repstr[0] = 'a';
	else
		repstr[0] = '-';

	if (status & EXPORTED_VG)
		repstr[1] = 'x';
	else
		repstr[1] = '-';

	field->report_string = repstr;
	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _vgstatus_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	char *repstr;

	if (!(repstr = pool_zalloc(rh->mem, 6))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (vg->status & LVM_WRITE)
		repstr[0] = 'w';
	else
		repstr[0] = 'r';

	if (vg->status & RESIZEABLE_VG)
		repstr[1] = 'z';
	else
		repstr[1] = '-';

	if (vg->status & EXPORTED_VG)
		repstr[2] = 'x';
	else
		repstr[2] = '-';

	if (vg->status & PARTIAL_VG)
		repstr[3] = 'p';
	else
		repstr[3] = '-';

	repstr[4] = _alloc_policy_char(vg->alloc);

	field->report_string = repstr;
	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _segtype_disp(struct report_handle *rh, struct field *field,
			 const void *data)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;

	if (seg->area_count == 1)
		field->report_string = "linear";
	else
		field->report_string = seg->segtype->ops->name(seg);
	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _origin_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	struct lv_segment *snap_seg;

	if ((snap_seg = find_cow(lv)))
		return _string_disp(rh, field, &snap_seg->origin->name);

	field->report_string = "";
	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _movepv_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	const char *name;
	struct list *segh;
	struct lv_segment *seg;

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct lv_segment);
		if (!(seg->status & PVMOVE))
			continue;
		name = dev_name(seg->area[0].u.pv.pv->dev);
		return _string_disp(rh, field, &name);
	}

	field->report_string = "";
	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _size32_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const uint32_t size = *(const uint32_t *) data;
	const char *disp;
	uint64_t *sortval;

	if (!*(disp = display_size(rh->cmd, (uint64_t) size, SIZE_UNIT))) {
		stack;
		return 0;
	}

	if (!(field->report_string = pool_strdup(rh->mem, disp))) {
		log_error("pool_strdup failed");
		return 0;
	}

	if (!(sortval = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	*sortval = (const uint64_t) size;
	field->sort_value = (const void *) sortval;

	return 1;
}

static int _size64_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const uint64_t size = *(const uint64_t *) data;
	const char *disp;
	uint64_t *sortval;

	if (!*(disp = display_size(rh->cmd, size, SIZE_UNIT))) {
		stack;
		return 0;
	}

	if (!(field->report_string = pool_strdup(rh->mem, disp))) {
		log_error("pool_strdup failed");
		return 0;
	}

	if (!(sortval = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	*sortval = size;
	field->sort_value = sortval;

	return 1;
}

static int _vgsize_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint64_t size;

	size = (uint64_t) vg->extent_count * vg->extent_size;

	return _size64_disp(rh, field, &size);
}

static int _segstart_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	uint64_t start;

	start = (uint64_t) seg->le * seg->lv->vg->extent_size;

	return _size64_disp(rh, field, &start);
}

static int _segsize_disp(struct report_handle *rh, struct field *field,
			 const void *data)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	uint64_t size;

	size = (uint64_t) seg->len * seg->lv->vg->extent_size;

	return _size64_disp(rh, field, &size);
}

static int _pvused_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t used;

	if (!pv->pe_count)
		used = 0LL;
	else
		used = (uint64_t) pv->pe_alloc_count * pv->pe_size;

	return _size64_disp(rh, field, &used);
}

static int _pvfree_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t freespace;

	if (!pv->pe_count)
		freespace = pv->size;
	else
		freespace = (uint64_t) (pv->pe_count - pv->pe_alloc_count) * pv->pe_size;

	return _size64_disp(rh, field, &freespace);
}

static int _pvsize_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t size;

	if (!pv->pe_count)
		size = pv->size;
	else
		size = (uint64_t) pv->pe_count * pv->pe_size;

	return _size64_disp(rh, field, &size);
}

static int _devsize_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct device *dev = *(const struct device **) data;
	uint64_t size;

	if (!dev_get_size(dev, &size))
		size = 0;

	return _size64_disp(rh, field, &size);
}

static int _vgfree_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint64_t freespace;

	freespace = (uint64_t) vg->free_count * vg->extent_size;

	return _size64_disp(rh, field, &freespace);
}

static int _uuid_disp(struct report_handle *rh, struct field *field,
		      const void *data)
{
	char *repstr = NULL;

	if (!(repstr = pool_alloc(rh->mem, 40))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!id_write_format((const struct id *) data, repstr, 40)) {
		stack;
		return 0;
	}

	field->report_string = repstr;
	field->sort_value = (const void *) field->report_string;

	return 1;
}

static int _uint32_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	const uint32_t value = *(const uint32_t *) data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = pool_zalloc(rh->mem, 12))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!(sortval = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (lvm_snprintf(repstr, 11, "%u", value) < 0) {
		log_error("uint32 too big: %u", value);
		return 0;
	}

	*sortval = (const uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

static int _int32_disp(struct report_handle *rh, struct field *field,
		       const void *data)
{
	const int32_t value = *(const int32_t *) data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = pool_zalloc(rh->mem, 13))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!(sortval = pool_alloc(rh->mem, sizeof(int64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (lvm_snprintf(repstr, 12, "%d", value) < 0) {
		log_error("int32 too big: %d", value);
		return 0;
	}

	*sortval = (const uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

static int _lvsegcount_disp(struct report_handle *rh, struct field *field,
			    const void *data)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	uint32_t count;

	count = list_size(&lv->segments);

	return _uint32_disp(rh, field, &count);
}

static int _snpercent_disp(struct report_handle *rh, struct field *field,
			   const void *data)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	struct lv_segment *snap_seg;
	struct lvinfo info;
	float snap_percent;
	uint64_t *sortval;
	char *repstr;

	if (!(sortval = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!(snap_seg = find_cow(lv)) ||
	    (lv_info(snap_seg->cow, &info, 0) && !info.exists)) {
		field->report_string = "";
		*sortval = UINT64_C(0);
		field->sort_value = sortval;
		return 1;
	}

	if (!lv_snapshot_percent(snap_seg->cow, &snap_percent)
	    || snap_percent < 0) {
		field->report_string = "100.00";
		*sortval = UINT64_C(100);
		field->sort_value = sortval;
		return 1;
	}

	if (!(repstr = pool_zalloc(rh->mem, 8))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (lvm_snprintf(repstr, 7, "%.2f", snap_percent) < 0) {
		log_error("snapshot percentage too large");
		return 0;
	}

	*sortval = snap_percent * UINT64_C(1000);
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

static int _copypercent_disp(struct report_handle *rh, struct field *field,
			     const void *data)
{
	struct logical_volume *lv = (struct logical_volume *) data;
	float percent;
	uint64_t *sortval;
	char *repstr;

	if (!(sortval = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if ((!(lv->status & PVMOVE) && !(lv->status & MIRRORED)) ||
	    !lv_mirror_percent(lv, 0, &percent, NULL)) {
		field->report_string = "";
		*sortval = UINT64_C(0);
		field->sort_value = sortval;
		return 1;
	}

	percent = copy_percent(lv);

	if (!(repstr = pool_zalloc(rh->mem, 8))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (lvm_snprintf(repstr, 7, "%.2f", percent) < 0) {
		log_error("copy percentage too large");
		return 0;
	}

	*sortval = percent * UINT64_C(1000);
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

/*
 * Import column definitions
 */

#define STR (FLD_STRING | FLD_ALIGN_LEFT)
#define NUM (FLD_NUMBER | FLD_ALIGN_RIGHT)
#define FIELD(type, strct, sorttype, head, field, width, func, id) {type, id, (off_t)((void *)&_dummy._ ## strct.field - (void *)&_dummy._ ## strct), head, width, sorttype, &_ ## func ## _disp},

static struct {
	report_type_t type;
	const char id[32];
	off_t offset;
	const char heading[32];
	int width;
	uint32_t flags;
	field_report_fn report_fn;
} _fields[] = {
#include "columns.h"
};

#undef STR
#undef NUM
#undef FIELD

const unsigned int _num_fields = sizeof(_fields) / sizeof(_fields[0]);

/*
 * Initialise report handle
 */
static int _field_match(struct report_handle *rh, const char *field, size_t len)
{
	uint32_t f, l;
	struct field_properties *fp;

	if (!len)
		return 0;

	for (f = 0; f < _num_fields; f++) {
		if ((!strncasecmp(_fields[f].id, field, len) &&
		     strlen(_fields[f].id) == len) ||
		    (l = strlen(rh->field_prefix),
		     !strncasecmp(rh->field_prefix, _fields[f].id, l) &&
		     !strncasecmp(_fields[f].id + l, field, len) &&
		     strlen(_fields[f].id) == l + len)) {
			rh->type |= _fields[f].type;
			if (!(fp = pool_zalloc(rh->mem, sizeof(*fp)))) {
				log_error("struct field_properties allocation "
					  "failed");
				return 0;
			}
			fp->field_num = f;
			fp->width = _fields[f].width;
			fp->flags = _fields[f].flags;

			/* Suppress snapshot percentage if not using driver */
			if (!activation()
			    && !strncmp(field, "snap_percent", len))
				fp->flags |= FLD_HIDDEN;

			list_add(&rh->field_props, &fp->list);
			return 1;
		}
	}

	return 0;
}

static int _add_sort_key(struct report_handle *rh, uint32_t field_num,
			 uint32_t flags)
{
	struct list *fh;
	struct field_properties *fp, *found = NULL;

	list_iterate(fh, &rh->field_props) {
		fp = list_item(fh, struct field_properties);

		if (fp->field_num == field_num) {
			found = fp;
			break;
		}
	}

	if (!found) {
		/* Add as a non-display field */
		if (!(found = pool_zalloc(rh->mem, sizeof(*found)))) {
			log_error("struct field_properties allocation failed");
			return 0;
		}

		rh->type |= _fields[field_num].type;
		found->field_num = field_num;
		found->width = _fields[field_num].width;
		found->flags = _fields[field_num].flags | FLD_HIDDEN;

		list_add(&rh->field_props, &found->list);
	}

	if (found->flags & FLD_SORT_KEY) {
		log_error("Ignoring duplicate sort field: %s",
			  _fields[field_num].id);
		return 1;
	}

	found->flags |= FLD_SORT_KEY;
	found->sort_posn = rh->keys_count++;
	found->flags |= flags;

	return 1;
}

static int _key_match(struct report_handle *rh, const char *key, size_t len)
{
	uint32_t f, l;
	uint32_t flags = 0;

	if (!len)
		return 0;

	if (*key == '+') {
		key++;
		len--;
		flags = FLD_ASCENDING;
	} else if (*key == '-') {
		key++;
		len--;
		flags = FLD_DESCENDING;
	} else
		flags = FLD_ASCENDING;

	if (!len) {
		log_error("Missing sort field name");
		return 0;
	}

	for (f = 0; f < _num_fields; f++) {
		if ((!strncasecmp(_fields[f].id, key, len) &&
		     strlen(_fields[f].id) == len) ||
		    (l = strlen(rh->field_prefix),
		     !strncasecmp(rh->field_prefix, _fields[f].id, l) &&
		     !strncasecmp(_fields[f].id + l, key, len) &&
		     strlen(_fields[f].id) == l + len)) {
			return _add_sort_key(rh, f, flags);
		}
	}

	return 0;
}

static int _parse_options(struct report_handle *rh, const char *format)
{
	const char *ws;		/* Word start */
	const char *we = format;	/* Word end */

	while (*we) {
		/* Allow consecutive commas */
		while (*we && *we == ',')
			we++;
		ws = we;
		while (*we && *we != ',')
			we++;
		if (!_field_match(rh, ws, (size_t) (we - ws))) {
			log_error("Unrecognised field: %.*s", (int) (we - ws),
				  ws);
			return 0;
		}
	}

	return 1;
}

static int _parse_keys(struct report_handle *rh, const char *keys)
{
	const char *ws;		/* Word start */
	const char *we = keys;	/* Word end */

	while (*we) {
		/* Allow consecutive commas */
		while (*we && *we == ',')
			we++;
		ws = we;
		while (*we && *we != ',')
			we++;
		if (!_key_match(rh, ws, (size_t) (we - ws))) {
			log_error("Unrecognised field: %.*s", (int) (we - ws),
				  ws);
			return 0;
		}
	}

	return 1;
}

void *report_init(struct cmd_context *cmd, const char *format, const char *keys,
		  report_type_t *report_type, const char *separator,
		  int aligned, int buffered, int headings)
{
	struct report_handle *rh;

	if (!(rh = pool_zalloc(cmd->mem, sizeof(*rh)))) {
		log_error("report_handle pool_zalloc failed");
		return 0;
	}

	rh->cmd = cmd;
	rh->type = *report_type;
	rh->separator = separator;

	if (aligned)
		rh->flags |= RH_ALIGNED;

	if (buffered)
		rh->flags |= RH_BUFFERED | RH_SORT_REQUIRED;

	if (headings)
		rh->flags |= RH_HEADINGS;

	list_init(&rh->field_props);
	list_init(&rh->rows);

	switch (rh->type) {
	case PVS:
		rh->field_prefix = "pv_";
		break;
	case LVS:
		rh->field_prefix = "lv_";
		break;
	case VGS:
		rh->field_prefix = "vg_";
		break;
	case SEGS:
		rh->field_prefix = "seg_";
		break;
	case PVSEGS:
		rh->field_prefix = "pvseg_";
		break;
	default:
		rh->field_prefix = "";
	}

	if (!(rh->mem = pool_create("report", 10 * 1024))) {
		log_error("Allocation of memory pool for report failed");
		return NULL;
	}

	/* Generate list of fields for output based on format string & flags */
	if (!_parse_options(rh, format))
		return NULL;

	if (!_parse_keys(rh, keys))
		return NULL;

	/* Ensure options selected are compatible */
	if (rh->type & SEGS)
		rh->type |= LVS;
	if (rh->type & PVSEGS)
		rh->type |= PVS;
	if ((rh->type & LVS) && (rh->type & PVS)) {
		log_error("Can't report LV and PV fields at the same time");
		return NULL;
	}

	/* Change report type if fields specified makes this necessary */
	if (rh->type & SEGS)
		*report_type = SEGS;
	else if (rh->type & LVS)
		*report_type = LVS;
	else if (rh->type & PVSEGS)
		*report_type = PVSEGS;
	else if (rh->type & PVS)
		*report_type = PVS;

	return rh;
}

void report_free(void *handle)
{
	struct report_handle *rh = handle;

	pool_destroy(rh->mem);

	return;
}

/*
 * Create a row of data for an object
 */
int report_object(void *handle, struct volume_group *vg,
		  struct logical_volume *lv, struct physical_volume *pv,
		  struct lv_segment *seg, struct pv_segment *pvseg)
{
	struct report_handle *rh = handle;
	struct list *fh;
	struct field_properties *fp;
	struct row *row;
	struct field *field;
	void *data = NULL;
	int skip;

	if (lv && pv) {
		log_error("report_object: One of *lv and *pv must be NULL!");
		return 0;
	}

	if (!(row = pool_zalloc(rh->mem, sizeof(*row)))) {
		log_error("struct row allocation failed");
		return 0;
	}

	row->rh = rh;

	if ((rh->flags & RH_SORT_REQUIRED) &&
	    !(row->sort_fields = pool_zalloc(rh->mem, sizeof(struct field *) *
					     rh->keys_count))) {
		log_error("row sort value structure allocation failed");
		return 0;
	}

	list_init(&row->fields);
	list_add(&rh->rows, &row->list);

	/* For each field to be displayed, call its report_fn */
	list_iterate(fh, &rh->field_props) {
		fp = list_item(fh, struct field_properties);

		skip = 0;

		if (!(field = pool_zalloc(rh->mem, sizeof(*field)))) {
			log_error("struct field allocation failed");
			return 0;
		}
		field->props = fp;

		switch (_fields[fp->field_num].type) {
		case LVS:
			data = (void *) lv + _fields[fp->field_num].offset;
			break;
		case VGS:
			if (!vg) {
				skip = 1;
				break;
			}
			data = (void *) vg + _fields[fp->field_num].offset;
			break;
		case PVS:
			data = (void *) pv + _fields[fp->field_num].offset;
			break;
		case SEGS:
			data = (void *) seg + _fields[fp->field_num].offset;
			break;
		case PVSEGS:
			data = (void *) pvseg + _fields[fp->field_num].offset;
		}

		if (skip) {
			field->report_string = "";
			field->sort_value = (const void *) field->report_string;
		} else if (!_fields[fp->field_num].report_fn(rh, field, data)) {
			log_error("report function failed for field %s",
				  _fields[fp->field_num].id);
			return 0;
		}

		if ((strlen(field->report_string) > field->props->width))
			field->props->width = strlen(field->report_string);

		if ((rh->flags & RH_SORT_REQUIRED) &&
		    (field->props->flags & FLD_SORT_KEY)) {
			(*row->sort_fields)[field->props->sort_posn] = field;
		}
		list_add(&row->fields, &field->list);
	}

	if (!(rh->flags & RH_BUFFERED))
		report_output(handle);

	return 1;
}

/* 
 * Print row of headings 
 */
static int _report_headings(void *handle)
{
	struct report_handle *rh = handle;
	struct list *fh;
	struct field_properties *fp;
	const char *heading;
	char buf[1024];

	if (rh->flags & RH_HEADINGS_PRINTED)
		return 1;

	rh->flags |= RH_HEADINGS_PRINTED;

	if (!(rh->flags & RH_HEADINGS))
		return 1;

	if (!pool_begin_object(rh->mem, 128)) {
		log_error("pool_begin_object failed for headings");
		return 0;
	}

	/* First heading line */
	list_iterate(fh, &rh->field_props) {
		fp = list_item(fh, struct field_properties);
		if (fp->flags & FLD_HIDDEN)
			continue;

		heading = _fields[fp->field_num].heading;
		if (rh->flags & RH_ALIGNED) {
			if (lvm_snprintf(buf, sizeof(buf), "%-*.*s",
					 fp->width, fp->width, heading) < 0) {
				log_error("snprintf heading failed");
				pool_end_object(rh->mem);
				return 0;
			}
			if (!pool_grow_object(rh->mem, buf, fp->width))
				goto bad;
		} else if (!pool_grow_object(rh->mem, heading, strlen(heading)))
			goto bad;

		if (!list_end(&rh->field_props, fh))
			if (!pool_grow_object(rh->mem, rh->separator,
					      strlen(rh->separator)))
				goto bad;
	}
	if (!pool_grow_object(rh->mem, "\0", 1)) {
		log_error("pool_grow_object failed");
		goto bad;
	}
	log_print("%s", (char *) pool_end_object(rh->mem));

	return 1;

      bad:
	log_error("Failed to generate report headings for printing");

	return 0;
}

/*
 * Sort rows of data
 */
static int _row_compare(const void *a, const void *b)
{
	const struct row *rowa = *(const struct row **) a;
	const struct row *rowb = *(const struct row **) b;
	const struct field *sfa, *sfb;
	int32_t cnt = -1;

	for (cnt = 0; cnt < rowa->rh->keys_count; cnt++) {
		sfa = (*rowa->sort_fields)[cnt];
		sfb = (*rowb->sort_fields)[cnt];
		if (sfa->props->flags & FLD_NUMBER) {
			const uint64_t numa =
			    *(const uint64_t *) sfa->sort_value;
			const uint64_t numb =
			    *(const uint64_t *) sfb->sort_value;

			if (numa == numb)
				continue;

			if (sfa->props->flags & FLD_ASCENDING) {
				return (numa > numb) ? 1 : -1;
			} else {	/* FLD_DESCENDING */
				return (numa < numb) ? 1 : -1;
			}
		} else {	/* FLD_STRING */
			const char *stra = (const char *) sfa->sort_value;
			const char *strb = (const char *) sfb->sort_value;
			int cmp = strcmp(stra, strb);

			if (!cmp)
				continue;

			if (sfa->props->flags & FLD_ASCENDING) {
				return (cmp > 0) ? 1 : -1;
			} else {	/* FLD_DESCENDING */
				return (cmp < 0) ? 1 : -1;
			}
		}
	}

	return 0;		/* Identical */
}

static int _sort_rows(struct report_handle *rh)
{
	struct row *(*rows)[];
	struct list *rowh;
	uint32_t count = 0;
	struct row *row;

	if (!(rows = pool_alloc(rh->mem, sizeof(**rows) *
				list_size(&rh->rows)))) {
		log_error("sort array allocation failed");
		return 0;
	}

	list_iterate(rowh, &rh->rows) {
		row = list_item(rowh, struct row);
		(*rows)[count++] = row;
	}

	qsort(rows, count, sizeof(**rows), _row_compare);

	list_init(&rh->rows);
	while (count--)
		list_add_h(&rh->rows, &(*rows)[count]->list);

	return 1;
}

/*
 * Produce report output
 */
int report_output(void *handle)
{
	struct report_handle *rh = handle;
	struct list *fh, *rowh, *ftmp, *rtmp;
	struct row *row = NULL;
	struct field *field;
	const char *repstr;
	char buf[4096];
	int width;

	if (list_empty(&rh->rows))
		return 1;

	/* Sort rows */
	if ((rh->flags & RH_SORT_REQUIRED))
		_sort_rows(rh);

	/* If headings not printed yet, calculate field widths and print them */
	if (!(rh->flags & RH_HEADINGS_PRINTED))
		_report_headings(rh);

	/* Print and clear buffer */
	list_iterate_safe(rowh, rtmp, &rh->rows) {
		if (!pool_begin_object(rh->mem, 512)) {
			log_error("pool_begin_object failed for row");
			return 0;
		}
		row = list_item(rowh, struct row);
		list_iterate_safe(fh, ftmp, &row->fields) {
			field = list_item(fh, struct field);
			if (field->props->flags & FLD_HIDDEN)
				continue;

			repstr = field->report_string;
			width = field->props->width;
			if (!(rh->flags & RH_ALIGNED)) {
				if (!pool_grow_object(rh->mem, repstr,
						      strlen(repstr)))
					goto bad;
			} else if (field->props->flags & FLD_ALIGN_LEFT) {
				if (lvm_snprintf(buf, sizeof(buf), "%-*.*s",
						 width, width, repstr) < 0) {
					log_error("snprintf repstr failed");
					pool_end_object(rh->mem);
					return 0;
				}
				if (!pool_grow_object(rh->mem, buf, width))
					goto bad;
			} else if (field->props->flags & FLD_ALIGN_RIGHT) {
				if (lvm_snprintf(buf, sizeof(buf), "%*.*s",
						 width, width, repstr) < 0) {
					log_error("snprintf repstr failed");
					pool_end_object(rh->mem);
					return 0;
				}
				if (!pool_grow_object(rh->mem, buf, width))
					goto bad;
			}

			if (!list_end(&row->fields, fh))
				if (!pool_grow_object(rh->mem, rh->separator,
						      strlen(rh->separator)))
					goto bad;
			list_del(&field->list);
		}
		if (!pool_grow_object(rh->mem, "\0", 1)) {
			log_error("pool_grow_object failed for row");
			return 0;
		}
		log_print("%s", (char *) pool_end_object(rh->mem));
		list_del(&row->list);
	}

	if (row)
		pool_free(rh->mem, row);

	return 1;

      bad:
	log_error("Failed to generate row for printing");
	return 0;
}
