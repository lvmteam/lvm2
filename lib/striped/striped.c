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
#include "segtypes.h"
#include "display.h"
#include "text_export.h"
#include "text_import.h"
#include "config.h"
#include "str_list.h"
#include "targets.h"
#include "lvm-string.h"
#include "activate.h"

static const char *_name(const struct lv_segment *seg)
{
	return (seg->area_count == 1) ? "linear" : seg->segtype->name;
}

static void _display(const struct lv_segment *seg)
{
	uint32_t s;

	if (seg->area_count == 1)
		display_stripe(seg, 0, "  ");
	else {
		log_print("  Stripes\t\t%u", seg->area_count);
		log_print("  Stripe size\t\t%u KB", seg->stripe_size / 2);

		for (s = 0; s < seg->area_count; s++) {
			log_print("  Stripe %d:", s);
			display_stripe(seg, s, "    ");
		}
	}
	log_print(" ");
}

static int _text_import_area_count(struct config_node *sn, uint32_t *area_count)
{
	if (!get_config_uint32(sn, "stripe_count", area_count)) {
		log_error("Couldn't read 'stripe_count' for "
			  "segment '%s'.", sn->key);
		return 0;
	}

	return 1;
}

static int _text_import(struct lv_segment *seg, const struct config_node *sn,
			struct hash_table *pv_hash)
{
	struct config_node *cn;

	if ((seg->area_count != 1) &&
	    !get_config_uint32(sn, "stripe_size", &seg->stripe_size)) {
		log_error("Couldn't read stripe_size for segment '%s'.",
			  sn->key);
		return 0;
	}

	if (!(cn = find_config_node(sn, "stripes"))) {
		log_error("Couldn't find stripes array for segment "
			  "'%s'.", sn->key);
		return 0;
	}

	seg->area_len /= seg->area_count;

	return text_import_areas(seg, sn, cn, pv_hash);
}

static int _text_export(const struct lv_segment *seg, struct formatter *f)
{

	outf(f, "stripe_count = %u%s", seg->area_count,
	     (seg->area_count == 1) ? "\t# linear" : "");

	if (seg->area_count > 1)
		out_size(f, (uint64_t) seg->stripe_size,
			 "stripe_size = %u", seg->stripe_size);

	return out_areas(f, seg, "stripe");
}

/*
 * Test whether two segments could be merged by the current merging code
 */
static int _segments_compatible(struct lv_segment *first,
				struct lv_segment *second)
{
	uint32_t width;
	unsigned s;

	if ((first->area_count != second->area_count) ||
	    (first->stripe_size != second->stripe_size)) return 0;

	for (s = 0; s < first->area_count; s++) {

		/* FIXME Relax this to first area type != second area type */
		/*       plus the additional AREA_LV checks needed */
		if ((first->area[s].type != AREA_PV) ||
		    (second->area[s].type != AREA_PV)) return 0;

		width = first->area_len;

		if ((first->area[s].u.pv.pv != second->area[s].u.pv.pv) ||
		    (first->area[s].u.pv.pe + width != second->area[s].u.pv.pe))
			return 0;
	}

	if (!str_list_lists_equal(&first->tags, &second->tags))
		return 0;

	return 1;
}

static int _merge_segments(struct lv_segment *seg1, struct lv_segment *seg2)
{
	if (!_segments_compatible(seg1, seg2))
		return 0;

	seg1->len += seg2->len;
	seg1->area_len += seg2->area_len;

	return 1;
}

#ifdef DEVMAPPER_SUPPORT
static int _compose_target_line(struct dev_manager *dm, struct pool *mem,
				struct config_tree *cft, void **target_state,
				struct lv_segment *seg, char *params,
				size_t paramsize, const char **target, int *pos,
				uint32_t *pvmove_mirror_count)
{
	/*   linear [device offset]+
	 *   striped #stripes stripe_size [device offset]+   */

	if (seg->area_count == 1)
		*target = "linear";
	else if (seg->area_count > 1) {
		*target = "striped";
		if ((*pos = lvm_snprintf(params, paramsize, "%u %u ",
					 seg->area_count,
					 seg->stripe_size)) < 0) {
			stack;
			return -1;
		}
	} else {
		log_error("Internal error: striped target with no stripes");
		return 0;
	}

	return compose_areas_line(dm, seg, params, paramsize, pos, 0u,
				  seg->area_count);
}

static int _target_present(void)
{
	static int checked = 0;
	static int present = 0;

	if (!checked)
		present = target_present("linear") && target_present("striped");

	checked = 1;
	return present;
}
#endif

static void _destroy(const struct segment_type *segtype)
{
	dbg_free((void *) segtype);
}

static struct segtype_handler _striped_ops = {
	name:_name,
	display:_display,
	text_import_area_count:_text_import_area_count,
	text_import:_text_import,
	text_export:_text_export,
	merge_segments:_merge_segments,
#ifdef DEVMAPPER_SUPPORT
	compose_target_line:_compose_target_line,
	target_present:_target_present,
#endif
	destroy:_destroy,
};

struct segment_type *init_striped_segtype(struct cmd_context *cmd)
{
	struct segment_type *segtype = dbg_malloc(sizeof(*segtype));

	if (!segtype) {
		stack;
		return NULL;
	}

	segtype->cmd = cmd;
	segtype->ops = &_striped_ops;
	segtype->name = "striped";
	segtype->private = NULL;
	segtype->flags =
	    SEG_CAN_SPLIT | SEG_AREAS_STRIPED | SEG_FORMAT1_SUPPORT;

	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}
