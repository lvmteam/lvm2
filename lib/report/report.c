/*
 * Copyright (C) 2002 Sistina Software
 *
 * This LVM library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This LVM library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this LVM library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 *
 */

#include "lib.h"
#include "metadata.h"
#include "report.h"
#include "toolcontext.h"
#include "pool.h"
#include "lvm-string.h"
#include "display.h"
#include "activate.h"

#include <sys/types.h>

/* 
 * For macro use
 */
static union {
	struct physical_volume _pv;
	struct logical_volume _lv;
	struct volume_group _vg;
	struct lv_segment _seg;
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
	char *field_prefix;
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

	char *report_string;	/* Formatted ready for display */
	void *sort_value;	/* Raw value for sorting */
};

struct row {
	struct list list;
	struct report_handle *rh;
	struct list fields;	/* Fields in display order */
	struct field *(*sort_fields)[];	/* Fields in sort order */
};

/*
 * Data-munging functions to prepare each data type for display and sorting
 */
static int _string_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	if (!(field->report_string = pool_strdup(rh->mem, *(char **) data))) {
		log_error("pool_strdup failed");
		return 0;
	}

	field->sort_value = (void *) field->report_string;

	return 1;
}

static int _dev_name_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	const char *name = dev_name(*(struct device **) data);

	return _string_disp(rh, field, &name);
}

static int _vgfmt_disp(struct report_handle *rh, struct field *field,
		       const void *data)
{
	struct volume_group *vg = (struct volume_group *) data;

	if (!vg->fid) {
		field->report_string = "";
		field->sort_value = (void *) field->report_string;
		return 1;
	}

	return _string_disp(rh, field, &vg->fid->fmt->name);
}

static int _pvfmt_disp(struct report_handle *rh, struct field *field,
		       const void *data)
{
	struct physical_volume *pv = (struct physical_volume *) data;

	if (!pv->fmt) {
		field->report_string = "";
		field->sort_value = (void *) field->report_string;
		return 1;
	}

	return _string_disp(rh, field, &pv->fmt->name);
}

static int _lvstatus_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	struct logical_volume *lv = (struct logical_volume *) data;
	struct dm_info info;

	if (!(field->report_string = pool_zalloc(rh->mem, 7))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (lv_is_origin(lv))
		field->report_string[0] = 'o';
	else if (find_cow(lv))
		field->report_string[0] = 's';
	else
		field->report_string[0] = '-';

	if (lv->status & LVM_WRITE)
		field->report_string[1] = 'w';
	else
		field->report_string[1] = 'r';

	if (lv->alloc == ALLOC_CONTIGUOUS)
		field->report_string[2] = 'c';
	else
		field->report_string[2] = 'n';

	if (lv->status & FIXED_MINOR)
		field->report_string[3] = 'm';	/* Fixed Minor */
	else
		field->report_string[3] = '-';

	if (lv_info(lv, &info) && info.exists) {
		if (info.suspended)
			field->report_string[4] = 's';	/* Suspended */
		else
			field->report_string[4] = 'a';	/* Active */
		if (info.open_count)
			field->report_string[5] = 'o';	/* Open */
		else
			field->report_string[5] = '-';
	} else {
		field->report_string[4] = '-';
		field->report_string[5] = '-';
	}

	field->sort_value = (void *) field->report_string;

	return 1;
}

static int _pvstatus_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	uint32_t status = *(uint32_t *) data;

	if (!(field->report_string = pool_zalloc(rh->mem, 4))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (status & ALLOCATABLE_PV)
		field->report_string[0] = 'a';
	else
		field->report_string[0] = '-';

	if (status & EXPORTED_VG)
		field->report_string[1] = 'x';
	else
		field->report_string[1] = '-';

	field->sort_value = (void *) field->report_string;

	return 1;
}

static int _vgstatus_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	uint32_t status = *(uint32_t *) data;

	if (!(field->report_string = pool_zalloc(rh->mem, 5))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (status & LVM_WRITE)
		field->report_string[0] = 'w';
	else
		field->report_string[0] = 'r';

	if (status & RESIZEABLE_VG)
		field->report_string[1] = 'z';
	else
		field->report_string[1] = '-';

	if (status & EXPORTED_VG)
		field->report_string[2] = 'x';
	else
		field->report_string[2] = '-';

	if (status & PARTIAL_VG)
		field->report_string[3] = 'p';
	else
		field->report_string[3] = '-';

	field->sort_value = (void *) field->report_string;

	return 1;
}

static int _segtype_disp(struct report_handle *rh, struct field *field,
			 const void *data)
{
	struct lv_segment *seg = (struct lv_segment *) data;

	if (seg->stripes == 1)
		field->report_string = "linear";
	else
		field->report_string = (char *) get_segtype_string(seg->type);
	field->sort_value = (void *) field->report_string;

	return 1;
}

static int _origin_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	struct logical_volume *lv = (struct logical_volume *) data;
	struct snapshot *snap;

	if ((snap = find_cow(lv)))
		return _string_disp(rh, field, &snap->origin->name);

	field->report_string = "";
	field->sort_value = (void *) field->report_string;

	return 1;
}

static int _size32_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	uint32_t size = *(uint32_t *) data;
	const char *disp;

	if (!*(disp = display_size(rh->cmd, (uint64_t) size / 2, SIZE_UNIT))) {
		stack;
		return 0;
	}

	if (!(field->report_string = pool_strdup(rh->mem, disp))) {
		log_error("pool_strdup failed");
		return 0;
	}

	if (!(field->sort_value = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	*(uint64_t *) field->sort_value = (uint64_t) size;

	return 1;
}

static int _size64_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	uint64_t size = *(uint64_t *) data;
	const char *disp;

	if (!*(disp = display_size(rh->cmd, size / 2, SIZE_UNIT))) {
		stack;
		return 0;
	}

	if (!(field->report_string = pool_strdup(rh->mem, disp))) {
		log_error("pool_strdup failed");
		return 0;
	}

	if (!(field->sort_value = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	*(uint64_t *) field->sort_value = size;

	return 1;
}

static int _vgsize_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	struct volume_group *vg = (struct volume_group *) data;
	uint64_t size;

	size = vg->extent_count * vg->extent_size;

	return _size64_disp(rh, field, &size);
}

static int _segstart_disp(struct report_handle *rh, struct field *field,
			  const void *data)
{
	struct lv_segment *seg = (struct lv_segment *) data;
	uint64_t start;

	start = seg->le * seg->lv->vg->extent_size;

	return _size64_disp(rh, field, &start);
}

static int _segsize_disp(struct report_handle *rh, struct field *field,
			 const void *data)
{
	struct lv_segment *seg = (struct lv_segment *) data;
	uint64_t size;

	size = seg->len * seg->lv->vg->extent_size;

	return _size64_disp(rh, field, &size);
}

static int _pvused_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	struct physical_volume *pv = (struct physical_volume *) data;
	uint64_t used;

	if (!pv->pe_count)
		used = 0LL;
	else
		used = pv->pe_alloc_count * pv->pe_size;

	return _size64_disp(rh, field, &used);
}

static int _pvfree_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	struct physical_volume *pv = (struct physical_volume *) data;
	uint64_t free;

	if (!pv->pe_count)
		free = pv->size;
	else
		free = (pv->pe_count - pv->pe_alloc_count) * pv->pe_size;

	return _size64_disp(rh, field, &free);
}

static int _pvsize_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	struct physical_volume *pv = (struct physical_volume *) data;
	uint64_t size;

	if (!pv->pe_count)
		size = pv->size;
	else
		size = pv->pe_count * pv->pe_size;

	return _size64_disp(rh, field, &size);
}

static int _vgfree_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	struct volume_group *vg = (struct volume_group *) data;
	uint64_t free;

	free = vg->free_count * vg->extent_size;

	return _size64_disp(rh, field, &free);
}

static int _uuid_disp(struct report_handle *rh, struct field *field,
		      const void *data)
{
	if (!(field->report_string = pool_alloc(rh->mem, 40))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!id_write_format((struct id *) data, field->report_string, 40)) {
		stack;
		return 0;
	}

	field->sort_value = (void *) field->report_string;

	return 1;
}

static int _uint32_disp(struct report_handle *rh, struct field *field,
			const void *data)
{
	uint32_t value = *(uint32_t *) data;

	if (!(field->report_string = pool_zalloc(rh->mem, 12))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!(field->sort_value = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	*(uint64_t *) field->sort_value = value;

	if (lvm_snprintf(field->report_string, 11, "%u", value) < 0) {
		log_error("uint32 too big: %u", value);
		return 0;
	}

	return 1;
}

static int _int32_disp(struct report_handle *rh, struct field *field,
		       const void *data)
{
	int32_t value = *(int32_t *) data;

	if (!(field->report_string = pool_zalloc(rh->mem, 13))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!(field->sort_value = pool_alloc(rh->mem, sizeof(int64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	*(int64_t *) field->sort_value = value;

	if (lvm_snprintf(field->report_string, 12, "%d", value) < 0) {
		log_error("int32 too big: %d", value);
		return 0;
	}

	return 1;
}

static int _lvsegcount_disp(struct report_handle *rh, struct field *field,
			    const void *data)
{
	struct logical_volume *lv = (struct logical_volume *) data;
	uint32_t count;

	count = list_size(&lv->segments);

	return _uint32_disp(rh, field, &count);
}

static int _snpercent_disp(struct report_handle *rh, struct field *field,
			   const void *data)
{
	struct logical_volume *lv = (struct logical_volume *) data;
	struct snapshot *snap;
	float snap_percent;

	if (!(field->sort_value = pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (!(snap = find_cow(lv))
	    || !lv_snapshot_percent(snap->cow, &snap_percent)) {
		field->report_string = "";
		*(uint64_t *) field->sort_value = 0LL;
		return 1;
	}

	if (!(field->report_string = pool_zalloc(rh->mem, 8))) {
		log_error("pool_alloc failed");
		return 0;
	}

	if (snap_percent == -1)
		snap_percent = 100;

	*(uint64_t *) field->sort_value = snap_percent * 1000;

	if (lvm_snprintf(field->report_string, 7, "%.2f", snap_percent) < 0) {
		log_error("snapshot percentage too large");
		return 0;
	}

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
	const char id[30];
	off_t offset;
	const char heading[30];
	int width;
	uint32_t flags;
	field_report_fn report_fn;
} _fields[] = {
#include "columns.h"
};

#undef STR
#undef NUM
#undef FIELD

const int _num_fields = sizeof(_fields) / sizeof(_fields[0]);

/*
 * Initialise report handle
 */
static int _field_match(struct report_handle *rh, const char *field, int len)
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
		if (!(fp = pool_zalloc(rh->mem, sizeof(*fp)))) {
			log_error("struct field_properties allocation failed");
			return 0;
		}

		rh->type |= _fields[field_num].type;
		fp->field_num = field_num;
		fp->width = _fields[field_num].width;
		fp->flags = _fields[field_num].flags | FLD_HIDDEN;

		list_add(&rh->field_props, &fp->list);
	}

	if (fp->flags & FLD_SORT_KEY) {
		log_error("Ignoring duplicate sort field: %s",
			  _fields[field_num].id);
		return 1;
	}

	fp->flags |= FLD_SORT_KEY;
	fp->sort_posn = rh->keys_count++;
	fp->flags |= flags;

	return 1;
}

static int _key_match(struct report_handle *rh, const char *key, int len)
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
		if (!_field_match(rh, ws, we - ws)) {
			log_error("Unrecognised field: %.*s", we - ws, ws);
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
		if (!_key_match(rh, ws, we - ws)) {
			log_error("Unrecognised field: %.*s", we - ws, ws);
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
	default:
		rh->field_prefix = "";
	}

	if (!(rh->mem = pool_create(10 * 1024))) {
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
	if ((rh->type & LVS) && (rh->type & PVS)) {
		log_error("Can't report LV and PV fields at the same time");
		return NULL;
	}

	/* Change report type if fields specified makes this necessary */
	if (rh->type & SEGS)
		*report_type = SEGS;
	else if (rh->type & LVS)
		*report_type = LVS;
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
		  struct lv_segment *seg)
{
	struct report_handle *rh = handle;
	struct list *fh;
	struct field_properties *fp;
	struct row *row;
	struct field *field;
	void *data;

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
			data = (void *) vg + _fields[fp->field_num].offset;
			break;
		case PVS:
			data = (void *) pv + _fields[fp->field_num].offset;
			break;
		case SEGS:
			data = (void *) seg + _fields[fp->field_num].offset;
		}

		if (!_fields[fp->field_num].report_fn(rh, field, data)) {
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
int report_headings(void *handle)
{
	struct report_handle *rh = handle;
	struct list *fh;
	struct field_properties *fp;

	if (rh->flags & RH_HEADINGS_PRINTED)
		return 1;

	if (!(rh->flags & RH_HEADINGS))
		goto out;

	/* First heading line */
	list_iterate(fh, &rh->field_props) {
		fp = list_item(fh, struct field_properties);
		if (fp->flags & FLD_HIDDEN)
			continue;
		if (rh->flags & RH_ALIGNED)
			printf("%-*.*s", fp->width, fp->width,
			       _fields[fp->field_num].heading);
		else
			printf("%s", _fields[fp->field_num].heading);
		if (!list_end(&rh->field_props, fh))
			printf("%s", rh->separator);
	}
	printf("\n");

      out:
	rh->flags |= RH_HEADINGS_PRINTED;

	return 1;
}

/*
 * Sort rows of data
 */
static int _row_compare(const void *a, const void *b)
{
	struct row *rowa = *(struct row **) a;
	struct row *rowb = *(struct row **) b;
	struct field *sfa, *sfb;
	int32_t cnt = -1;

	for (cnt = 0; cnt < rowa->rh->keys_count; cnt++) {
		sfa = (*rowa->sort_fields)[cnt];
		sfb = (*rowb->sort_fields)[cnt];
		if (sfa->props->flags & FLD_NUMBER) {
			uint64_t numa = *(uint64_t *) sfa->sort_value;
			uint64_t numb = *(uint64_t *) sfb->sort_value;

			if (numa == numb)
				continue;

			if (sfa->props->flags & FLD_ASCENDING) {
				return (numa > numb) ? 1 : -1;
			} else {	/* FLD_DESCENDING */
				return (numa < numb) ? 1 : -1;
			}
		} else {	/* FLD_STRING */
			char *stra = (char *) sfa->sort_value;
			char *strb = (char *) sfb->sort_value;
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

	if (list_empty(&rh->rows))
		return 1;

	/* Sort rows */
	if ((rh->flags & RH_SORT_REQUIRED))
		_sort_rows(rh);

	/* If headings not printed yet, calculate field widths and print them */
	if (!(rh->flags & RH_HEADINGS_PRINTED))
		report_headings(rh);

	/* Print and clear buffer */
	list_iterate_safe(rowh, rtmp, &rh->rows) {
		row = list_item(rowh, struct row);
		list_iterate_safe(fh, ftmp, &row->fields) {
			field = list_item(fh, struct field);
			if (field->props->flags & FLD_HIDDEN)
				continue;
			if (!(rh->flags & RH_ALIGNED))
				printf("%s", field->report_string);
			else if (field->props->flags & FLD_ALIGN_LEFT)
				printf("%-*.*s", field->props->width,
				       field->props->width,
				       field->report_string);
			else if (field->props->flags & FLD_ALIGN_RIGHT)
				printf("%*.*s", field->props->width,
				       field->props->width,
				       field->report_string);
			if (!list_end(&row->fields, fh))
				printf("%s", rh->separator);
			list_del(&field->list);
		}
		printf("\n");
		list_del(&row->list);
	}

	if (row)
		pool_free(rh->mem, row);

	return 1;
}
