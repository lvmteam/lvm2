/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
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
#include "import-export.h"
#include "metadata.h"
#include "hash.h"
#include "pool.h"
#include "display.h"
#include "lvm-string.h"

#include <stdarg.h>
#include <time.h>
#include <sys/utsname.h>

struct formatter;
typedef int (*out_with_comment_fn) (struct formatter * f, const char *comment,
				    const char *fmt, va_list ap);
typedef void (*nl_fn) (struct formatter * f);
/*
 * The first half of this file deals with
 * exporting the vg, ie. writing it to a file.
 */
struct formatter {
	struct pool *mem;	/* pv names allocated from here */
	struct hash_table *pv_names;	/* dev_name -> pv_name (eg, pv1) */

	union {
		FILE *fp;	/* where we're writing to */
		struct {
			char *buf;
			uint32_t size;
			uint32_t used;
		} buf;
	} data;

	out_with_comment_fn out_with_comment;
	nl_fn nl;

	int indent;		/* current level of indentation */
	int error;
	int header;		/* 1 => comments at start; 0 => end */
};

static struct utsname _utsname;

static void _init(void)
{
	static int _initialised = 0;

	if (_initialised)
		return;

	if (uname(&_utsname)) {
		log_error("uname failed: %s", strerror(errno));
		memset(&_utsname, 0, sizeof(_utsname));
	}

	_initialised = 1;
}

/*
 * Formatting functions.
 */
static int _out_size(struct formatter *f, uint64_t size, const char *fmt, ...)
    __attribute__ ((format(printf, 3, 4)));

static int _out_hint(struct formatter *f, const char *fmt, ...)
    __attribute__ ((format(printf, 2, 3)));

static int _out(struct formatter *f, const char *fmt, ...)
    __attribute__ ((format(printf, 2, 3)));

#define MAX_INDENT 5
static void _inc_indent(struct formatter *f)
{
	if (++f->indent > MAX_INDENT)
		f->indent = MAX_INDENT;
}

static void _dec_indent(struct formatter *f)
{
	if (!f->indent--) {
		log_error("Internal error tracking indentation");
		f->indent = 0;
	}
}

/*
 * Newline function for prettier layout.
 */
static void _nl_file(struct formatter *f)
{
	fprintf(f->data.fp, "\n");
}

static void _nl_raw(struct formatter *f)
{
	if (f->data.buf.used >= f->data.buf.size - 1)
		return;

	*f->data.buf.buf = '\n';
	f->data.buf.buf += 1;
	f->data.buf.used += 1;
	*f->data.buf.buf = '\0';

	return;
}

#define COMMENT_TAB 6
static int _out_with_comment_file(struct formatter *f, const char *comment,
				  const char *fmt, va_list ap)
{
	int i;
	char white_space[MAX_INDENT + 1];

	if (ferror(f->data.fp))
		return 0;

	for (i = 0; i < f->indent; i++)
		white_space[i] = '\t';
	white_space[i] = '\0';
	fprintf(f->data.fp, white_space);
	i = vfprintf(f->data.fp, fmt, ap);

	if (comment) {
		/*
		 * line comments up if possible.
		 */
		i += 8 * f->indent;
		i /= 8;
		i++;

		do
			fputc('\t', f->data.fp);

		while (++i < COMMENT_TAB);

		fprintf(f->data.fp, comment);
	}
	fputc('\n', f->data.fp);

	return 1;
}

static int _out_with_comment_raw(struct formatter *f, const char *comment,
				 const char *fmt, va_list ap)
{
	int n;

	n = vsnprintf(f->data.buf.buf, f->data.buf.size - f->data.buf.used,
		      fmt, ap);

	if (n < 0 || (n > f->data.buf.size - f->data.buf.used - 1))
		return 0;

	f->data.buf.buf += n;
	f->data.buf.used += n;

	f->nl(f);

	return 1;
}

/*
 * Formats a string, converting a size specified
 * in 512-byte sectors to a more human readable
 * form (eg, megabytes).  We may want to lift this
 * for other code to use.
 */
static int _sectors_to_units(uint64_t sectors, char *buffer, size_t s)
{
	static const char *_units[] = {
		"Kilobytes",
		"Megabytes",
		"Gigabytes",
		"Terabytes",
		NULL
	};

	int i;
	double d = (double) sectors;

	/* to convert to K */
	d /= 2.0;

	for (i = 0; (d > 1024.0) && _units[i]; i++)
		d /= 1024.0;

	return lvm_snprintf(buffer, s, "# %g %s", d, _units[i]) > 0;
}

/*
 * Appends a comment giving a size in more easily
 * readable form (eg, 4M instead of 8096).
 */
static int _out_size(struct formatter *f, uint64_t size, const char *fmt, ...)
{
	char buffer[64];
	va_list ap;
	int r;

	if (!_sectors_to_units(size, buffer, sizeof(buffer)))
		return 0;

	va_start(ap, fmt);
	r = f->out_with_comment(f, buffer, fmt, ap);
	va_end(ap);

	return r;
}

/*
 * Appends a comment indicating that the line is
 * only a hint.
 */
static int _out_hint(struct formatter *f, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = f->out_with_comment(f, "# Hint only", fmt, ap);
	va_end(ap);

	return r;
}

/*
 * The normal output function.
 */
static int _out(struct formatter *f, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = f->out_with_comment(f, NULL, fmt, ap);
	va_end(ap);

	return r;
}

#define _outf(args...) do {if (!_out(args)) {stack; return 0;}} while (0)

static int _print_header(struct formatter *f,
			 struct volume_group *vg, const char *desc)
{
	time_t t;

	t = time(NULL);

	_outf(f, "# Generated by LVM2: %s", ctime(&t));
	_outf(f, CONTENTS_FIELD " = \"" CONTENTS_VALUE "\"");
	_outf(f, FORMAT_VERSION_FIELD " = %d", FORMAT_VERSION_VALUE);
	f->nl(f);

	_outf(f, "description = \"%s\"", desc);
	f->nl(f);
	_outf(f, "creation_host = \"%s\"\t# %s %s %s %s %s", _utsname.nodename,
	      _utsname.sysname, _utsname.nodename, _utsname.release,
	      _utsname.version, _utsname.machine);
	_outf(f, "creation_time = %lu\t# %s", t, ctime(&t));

	return 1;
}

static int _print_vg(struct formatter *f, struct volume_group *vg)
{
	char buffer[4096];

	if (!id_write_format(&vg->id, buffer, sizeof(buffer))) {
		stack;
		return 0;
	}

	_outf(f, "id = \"%s\"", buffer);

	_outf(f, "seqno = %u", vg->seqno);

	if (!print_flags(vg->status, VG_FLAGS, buffer, sizeof(buffer))) {
		stack;
		return 0;
	}
	_outf(f, "status = %s", buffer);

	if (!list_empty(&vg->tags)) {
		if (!print_tags(&vg->tags, buffer, sizeof(buffer))) {
			stack;
			return 0;
		}
		_outf(f, "tags = %s", buffer);
	}

	if (vg->system_id && *vg->system_id)
		_outf(f, "system_id = \"%s\"", vg->system_id);

	if (!_out_size(f, (uint64_t) vg->extent_size, "extent_size = %u",
		       vg->extent_size)) {
		stack;
		return 0;
	}
	_outf(f, "max_lv = %u", vg->max_lv);
	_outf(f, "max_pv = %u", vg->max_pv);

	return 1;
}

/*
 * Get the pv%d name from the formatters hash
 * table.
 */
static inline const char *_get_pv_name(struct formatter *f,
				       struct physical_volume *pv)
{
	return (pv) ? (const char *)
	    hash_lookup(f->pv_names, dev_name(pv->dev)) : "Missing";
}

static int _print_pvs(struct formatter *f, struct volume_group *vg)
{
	struct list *pvh;
	struct physical_volume *pv;
	char buffer[4096];
	const char *name;

	_outf(f, "physical_volumes {");
	_inc_indent(f);

	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;

		if (!(name = _get_pv_name(f, pv))) {
			stack;
			return 0;
		}

		f->nl(f);
		_outf(f, "%s {", name);
		_inc_indent(f);

		if (!id_write_format(&pv->id, buffer, sizeof(buffer))) {
			stack;
			return 0;
		}

		_outf(f, "id = \"%s\"", buffer);
		if (!_out_hint(f, "device = \"%s\"", dev_name(pv->dev))) {
			stack;
			return 0;
		}
		f->nl(f);

		if (!print_flags(pv->status, PV_FLAGS, buffer, sizeof(buffer))) {
			stack;
			return 0;
		}
		_outf(f, "status = %s", buffer);

		if (!list_empty(&pv->tags)) {
			if (!print_tags(&pv->tags, buffer, sizeof(buffer))) {
				stack;
				return 0;
			}
			_outf(f, "tags = %s", buffer);
		}

		_outf(f, "pe_start = %" PRIu64, pv->pe_start);
		if (!_out_size(f, vg->extent_size * (uint64_t) pv->pe_count,
			       "pe_count = %u", pv->pe_count)) {
			stack;
			return 0;
		}

		_dec_indent(f);
		_outf(f, "}");
	}

	_dec_indent(f);
	_outf(f, "}");
	return 1;
}

static int _print_segment(struct formatter *f, struct volume_group *vg,
			  int count, struct lv_segment *seg)
{
	unsigned int s;
	const char *name;
	const char *type;
	char buffer[4096];

	_outf(f, "segment%u {", count);
	_inc_indent(f);

	_outf(f, "start_extent = %u", seg->le);
	if (!_out_size(f, (uint64_t) seg->len * vg->extent_size,
		       "extent_count = %u", seg->len)) {
		stack;
		return 0;
	}

	f->nl(f);
	_outf(f, "type = \"%s\"", get_segtype_string(seg->type));

	if (!list_empty(&seg->tags)) {
		if (!print_tags(&seg->tags, buffer, sizeof(buffer))) {
			stack;
			return 0;
		}
		_outf(f, "tags = %s", buffer);
	}

	switch (seg->type) {
	case SEG_SNAPSHOT:
		_outf(f, "chunk_size = %u", seg->chunk_size);
		_outf(f, "origin = \"%s\"", seg->origin->name);
		_outf(f, "cow_store = \"%s\"", seg->cow->name);
		break;

	case SEG_MIRRORED:
	case SEG_STRIPED:
		type = (seg->type == SEG_MIRRORED) ? "mirror" : "stripe";
		_outf(f, "%s_count = %u%s", type, seg->area_count,
		      (seg->area_count == 1) ? "\t# linear" : "");

		if ((seg->type == SEG_MIRRORED) && (seg->status & PVMOVE))
			_out_size(f, (uint64_t) seg->extents_moved,
				  "extents_moved = %u", seg->extents_moved);

		if ((seg->type == SEG_STRIPED) && (seg->area_count > 1))
			_out_size(f, (uint64_t) seg->stripe_size,
				  "stripe_size = %u", seg->stripe_size);

		f->nl(f);

		_outf(f, "%ss = [", type);
		_inc_indent(f);

		for (s = 0; s < seg->area_count; s++) {
			switch (seg->area[s].type) {
			case AREA_PV:
				if (!(name = _get_pv_name(f, seg->
							  area[s].u.pv.pv))) {
					stack;
					return 0;
				}

				_outf(f, "\"%s\", %u%s", name,
				      seg->area[s].u.pv.pe,
				      (s == seg->area_count - 1) ? "" : ",");
				break;
			case AREA_LV:
				_outf(f, "\"%s\", %u%s",
				      seg->area[s].u.lv.lv->name,
				      seg->area[s].u.lv.le,
				      (s == seg->area_count - 1) ? "" : ",");
			}
		}

		_dec_indent(f);
		_outf(f, "]");
		break;
	}

	_dec_indent(f);
	_outf(f, "}");

	return 1;
}

static int _count_segments(struct logical_volume *lv)
{
	int r = 0;
	struct list *segh;

	list_iterate(segh, &lv->segments)
	    r++;

	return r;
}

static int _print_snapshot(struct formatter *f, struct snapshot *snap,
			   unsigned int count)
{
	char buffer[256];
	struct lv_segment seg;

	f->nl(f);

	_outf(f, "snapshot%u {", count);
	_inc_indent(f);

	if (!id_write_format(&snap->id, buffer, sizeof(buffer))) {
		stack;
		return 0;
	}

	_outf(f, "id = \"%s\"", buffer);
	if (!print_flags(LVM_READ | LVM_WRITE | VISIBLE_LV, LV_FLAGS,
			 buffer, sizeof(buffer))) {
		stack;
		return 0;
	}

	_outf(f, "status = %s", buffer);
	_outf(f, "segment_count = 1");

	f->nl(f);

	seg.type = SEG_SNAPSHOT;
	seg.le = 0;
	seg.len = snap->origin->le_count;
	seg.origin = snap->origin;
	seg.cow = snap->cow;
	seg.chunk_size = snap->chunk_size;

	/* Can't tag a snapshot independently of its origin */
	list_init(&seg.tags);

	if (!_print_segment(f, snap->origin->vg, 1, &seg)) {
		stack;
		return 0;
	}

	_dec_indent(f);
	_outf(f, "}");

	return 1;
}

static int _print_snapshots(struct formatter *f, struct volume_group *vg)
{
	struct list *sh;
	struct snapshot *s;
	unsigned int count = 0;

	list_iterate(sh, &vg->snapshots) {
		s = list_item(sh, struct snapshot_list)->snapshot;

		if (!_print_snapshot(f, s, count++)) {
			stack;
			return 0;
		}
	}

	return 1;
}

static int _print_lvs(struct formatter *f, struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	struct lv_segment *seg;
	char buffer[4096];
	int seg_count;

	/*
	 * Don't bother with an lv section if there are no lvs.
	 */
	if (list_empty(&vg->lvs))
		return 1;

	_outf(f, "logical_volumes {");
	_inc_indent(f);

	list_iterate(lvh, &vg->lvs) {
		lv = list_item(lvh, struct lv_list)->lv;

		f->nl(f);
		_outf(f, "%s {", lv->name);
		_inc_indent(f);

		/* FIXME: Write full lvid */
		if (!id_write_format(&lv->lvid.id[1], buffer, sizeof(buffer))) {
			stack;
			return 0;
		}

		_outf(f, "id = \"%s\"", buffer);

		if (!print_flags(lv->status, LV_FLAGS, buffer, sizeof(buffer))) {
			stack;
			return 0;
		}
		_outf(f, "status = %s", buffer);

		if (!list_empty(&lv->tags)) {
			if (!print_tags(&lv->tags, buffer, sizeof(buffer))) {
				stack;
				return 0;
			}
			_outf(f, "tags = %s", buffer);
		}

		if (lv->alloc != ALLOC_DEFAULT)
			_outf(f, "allocation_policy = \"%s\"",
			      get_alloc_string(lv->alloc));
		if (lv->read_ahead)
			_outf(f, "read_ahead = %u", lv->read_ahead);
		if (lv->major >= 0)
			_outf(f, "major = %d", lv->major);
		if (lv->minor >= 0)
			_outf(f, "minor = %d", lv->minor);
		_outf(f, "segment_count = %u", _count_segments(lv));
		f->nl(f);

		seg_count = 1;
		list_iterate_items(seg, &lv->segments) {
			if (!_print_segment(f, vg, seg_count++, seg)) {
				stack;
				return 0;
			}
		}

		_dec_indent(f);
		_outf(f, "}");
	}

	if (!_print_snapshots(f, vg)) {
		stack;
		return 0;
	}

	_dec_indent(f);
	_outf(f, "}");

	return 1;
}

/*
 * In the text format we refer to pv's as 'pv1',
 * 'pv2' etc.  This function builds a hash table
 * to enable a quick lookup from device -> name.
 */
static int _build_pv_names(struct formatter *f, struct volume_group *vg)
{
	int count = 0;
	struct list *pvh;
	struct physical_volume *pv;
	char buffer[32], *name;

	if (!(f->mem = pool_create(512))) {
		stack;
		goto bad;
	}

	if (!(f->pv_names = hash_create(128))) {
		stack;
		goto bad;
	}

	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;

		/* FIXME But skip if there's already an LV called pv%d ! */
		if (lvm_snprintf(buffer, sizeof(buffer), "pv%d", count++) < 0) {
			stack;
			goto bad;
		}

		if (!(name = pool_strdup(f->mem, buffer))) {
			stack;
			goto bad;
		}

		if (!hash_insert(f->pv_names, dev_name(pv->dev), name)) {
			stack;
			goto bad;
		}
	}

	return 1;

      bad:
	if (f->mem)
		pool_destroy(f->mem);

	if (f->pv_names)
		hash_destroy(f->pv_names);

	return 0;
}

static int _text_vg_export(struct formatter *f,
			   struct volume_group *vg, const char *desc)
{
	int r = 0;

	if (!_build_pv_names(f, vg)) {
		stack;
		goto out;
	}
#define fail do {stack; goto out;} while(0)

	if (f->header && !_print_header(f, vg, desc))
		fail;

	if (!_out(f, "%s {", vg->name))
		fail;

	_inc_indent(f);

	if (!_print_vg(f, vg))
		fail;

	f->nl(f);
	if (!_print_pvs(f, vg))
		fail;

	f->nl(f);
	if (!_print_lvs(f, vg))
		fail;

	_dec_indent(f);
	if (!_out(f, "}"))
		fail;

	if (!f->header && !_print_header(f, vg, desc))
		fail;

#undef fail
	r = 1;

      out:
	if (f->mem)
		pool_destroy(f->mem);

	if (f->pv_names)
		hash_destroy(f->pv_names);

	return r;
}

int text_vg_export_file(struct volume_group *vg, const char *desc, FILE *fp)
{
	struct formatter *f;
	int r;

	_init();

	if (!(f = dbg_malloc(sizeof(*f)))) {
		stack;
		return 0;
	}

	memset(f, 0, sizeof(*f));
	f->data.fp = fp;
	f->indent = 0;
	f->header = 1;
	f->out_with_comment = &_out_with_comment_file;
	f->nl = &_nl_file;

	r = _text_vg_export(f, vg, desc);
	if (r)
		r = !ferror(f->data.fp);
	dbg_free(f);
	return r;
}

/* Returns amount of buffer used incl. terminating NUL */
int text_vg_export_raw(struct volume_group *vg, const char *desc, char *buf,
		       uint32_t size)
{
	struct formatter *f;
	int r;

	_init();

	if (!(f = dbg_malloc(sizeof(*f)))) {
		stack;
		return 0;
	}

	memset(f, 0, sizeof(*f));
	f->data.buf.buf = buf;
	f->data.buf.size = size;
	f->indent = 0;
	f->header = 0;
	f->out_with_comment = &_out_with_comment_raw;
	f->nl = &_nl_raw;

	if (!_text_vg_export(f, vg, desc)) {
		stack;
		r = 0;
		goto out;
	}

	r = f->data.buf.used + 1;

      out:
	dbg_free(f);
	return r;
}

#undef _outf
