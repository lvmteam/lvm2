/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.  
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
#include "pool.h"
#include "list.h"
#include "toolcontext.h"
#include "metadata.h"
#include "segtype.h"
#include "text_export.h"
#include "config.h"
#include "activate.h"

static const char *_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static int _text_import(struct lv_segment *seg, const struct config_node *sn,
			struct hash_table *pv_hash)
{
	uint32_t chunk_size;
	const char *org_name, *cow_name;
	struct logical_volume *org, *cow;

	seg->lv->status |= SNAPSHOT;

	if (!get_config_uint32(sn, "chunk_size", &chunk_size)) {
		log_error("Couldn't read chunk size for snapshot.");
		return 0;
	}

	log_suppress(1);

	if (!(cow_name = find_config_str(sn, "cow_store", NULL))) {
		log_suppress(0);
		log_error("Snapshot cow storage not specified.");
		return 0;
	}

	if (!(org_name = find_config_str(sn, "origin", NULL))) {
		log_suppress(0);
		log_error("Snapshot origin not specified.");
		return 0;
	}

	log_suppress(0);

	if (!(cow = find_lv(seg->lv->vg, cow_name))) {
		log_error("Unknown logical volume specified for "
			  "snapshot cow store.");
		return 0;
	}

	if (!(org = find_lv(seg->lv->vg, org_name))) {
		log_error("Unknown logical volume specified for "
			  "snapshot origin.");
		return 0;
	}

	if (!vg_add_snapshot(org, cow, 1, &seg->lv->lvid.id[1], chunk_size)) {
		stack;
		return 0;
	}

	return 1;
}

static int _text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "chunk_size = %u", seg->chunk_size);
	outf(f, "origin = \"%s\"", seg->origin->name);
	outf(f, "cow_store = \"%s\"", seg->cow->name);

	return 1;
}

#ifdef DEVMAPPER_SUPPORT
static int _target_percent(void **target_state, struct pool *mem,
			   struct config_tree *cft, struct lv_segment *seg,
			   char *params, uint64_t *total_numerator,
			   uint64_t *total_denominator, float *percent)
{
	float percent2;
	uint64_t numerator, denominator;

	if (index(params, '/')) {
		if (sscanf(params, "%" PRIu64 "/%" PRIu64,
			   &numerator, &denominator) == 2) {
			*total_numerator += numerator;
			*total_denominator += denominator;
		}
	} else if (sscanf(params, "%f", &percent2) == 1) {
		*percent += percent2;
		*percent /= 2;
	}

	return 1;
}

static int _target_present(void)
{
	static int checked = 0;
	static int present = 0;

	if (!checked)
		present = target_present("snapshot") &&
		    target_present("snapshot-origin");

	checked = 1;

	return present;
}
#endif

static void _destroy(const struct segment_type *segtype)
{
	dbg_free((void *) segtype);
}

static struct segtype_handler _snapshot_ops = {
	name:_name,
	text_import:_text_import,
	text_export:_text_export,
#ifdef DEVMAPPER_SUPPORT
	target_percent:_target_percent,
	target_present:_target_present,
#endif
	destroy:_destroy,
};

#ifdef SNAPSHOT_INTERNAL
struct segment_type *init_snapshot_segtype(struct cmd_context *cmd)
#else				/* Shared */
struct segment_type *init_segtype(struct cmd_context *cmd);
struct segment_type *init_segtype(struct cmd_context *cmd)
#endif
{
	struct segment_type *segtype = dbg_malloc(sizeof(*segtype));

	if (!segtype) {
		stack;
		return NULL;
	}

	segtype->cmd = cmd;
	segtype->ops = &_snapshot_ops;
	segtype->name = "snapshot";
	segtype->private = NULL;
	segtype->flags = SEG_SNAPSHOT;

	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}
