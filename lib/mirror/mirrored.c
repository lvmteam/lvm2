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
 *
 */

#include "lib.h"
#include "pool.h"
#include "list.h"
#include "toolcontext.h"
#include "metadata.h"
#include "segtypes.h"
#include "display.h"
#include "text_export.h"
#include "text_import.h"
#include "config.h"
#include "defaults.h"
#include "lvm-string.h"
#include "targets.h"

enum {
	MIRR_DISABLED,
	MIRR_RUNNING,
	MIRR_COMPLETED
};

struct mirror_state {
	uint32_t region_size;
};

static const char *_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static void _display(const struct lv_segment *seg)
{
	log_print("  Mirrors\t\t%u", seg->area_count);
	log_print("  Mirror size\t\t%u", seg->area_len);
	log_print("  Mirror original:");
	display_stripe(seg, 0, "    ");
	log_print("  Mirror destination:");
	display_stripe(seg, 1, "    ");
	log_print(" ");

}

static int _text_import_area_count(struct config_node *sn, uint32_t *area_count)
{
	if (!get_config_uint32(sn, "mirror_count", area_count)) {
		log_error("Couldn't read 'mirror_count' for "
			  "segment '%s'.", sn->key);
		return 0;
	}

	return 1;
}

static int _text_import(struct lv_segment *seg, const struct config_node *sn,
			struct hash_table *pv_hash)
{
	const struct config_node *cn;

	if (find_config_node(sn, "extents_moved")) {
		if (get_config_uint32(sn, "extents_moved", &seg->extents_moved))
			seg->status |= PVMOVE;
		else {
			log_error("Couldn't read 'extents_moved' for "
				  "segment '%s'.", sn->key);
			return 0;
		}
	}

	if (!(cn = find_config_node(sn, "mirrors"))) {
		log_error("Couldn't find mirrors array for segment "
			  "'%s'.", sn->key);
		return 0;
	}

	return text_import_areas(seg, sn, cn, pv_hash);
}

static int _text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "mirror_count = %u", seg->area_count);
	if (seg->status & PVMOVE)
		out_size(f, (uint64_t) seg->extents_moved,
			 "extents_moved = %u", seg->extents_moved);

	return out_areas(f, seg, "mirror");
}

#ifdef DEVMAPPER_SUPPORT
static struct mirror_state *_init_target(struct pool *mem,
					 struct config_tree *cft)
{
	struct mirror_state *mirr_state;

	if (!(mirr_state = pool_alloc(mem, sizeof(*mirr_state)))) {
		log_error("struct mirr_state allocation failed");
		return NULL;
	}

	mirr_state->region_size = 2 *
	    find_config_int(cft->root,
			    "activation/mirror_region_size",
			    DEFAULT_MIRROR_REGION_SIZE);

	return mirr_state;
}

static int _compose_target_line(struct dev_manager *dm, struct pool *mem,
				struct config_tree *cft, void **target_state,
				struct lv_segment *seg, char *params,
				size_t paramsize, const char **target, int *pos,
				uint32_t *pvmove_mirror_count)
{
	struct mirror_state *mirr_state;
	int mirror_status = MIRR_RUNNING;
	int areas = seg->area_count;
	int start_area = 0u;

	if (!*target_state)
		*target_state = _init_target(mem, cft);

	mirr_state = *target_state;

	/*   mirror  log_type #log_params [log_params]* 
	 *           #mirrors [device offset]+
	 */
	if (seg->status & PVMOVE) {
		if (seg->extents_moved == seg->area_len) {
			mirror_status = MIRR_COMPLETED;
			start_area = 1;
		} else if (*pvmove_mirror_count++) {
			mirror_status = MIRR_DISABLED;
			areas = 1;
		}
	}

	if (mirror_status != MIRR_RUNNING) {
		*target = "linear";
	} else {
		*target = "mirror";
		if ((*pos = lvm_snprintf(params, paramsize, "core 1 %u %u ",
					 mirr_state->region_size, areas)) < 0) {
			stack;
			return -1;
		}
	}

	return compose_areas_line(dm, seg, params, paramsize, pos, start_area,
				  areas);

}

static int _target_percent(void **target_state, struct pool *mem,
			   struct config_tree *cft, struct lv_segment *seg,
			   char *params, uint64_t *total_numerator,
			   uint64_t *total_denominator, float *percent)
{
	struct mirror_state *mirr_state;
	uint64_t numerator, denominator;

	if (!*target_state)
		*target_state = _init_target(mem, cft);

	mirr_state = *target_state;

	log_debug("Mirror status: %s", params);
	if (sscanf(params, "%*d %*x:%*x %*x:%*x %" PRIu64
		   "/%" PRIu64, &numerator, &denominator) != 2) {
		log_error("Failure parsing mirror status: %s", params);
		return 0;
	}
	*total_numerator += numerator;
	*total_denominator += denominator;

	if (seg && (seg->status & PVMOVE))
		seg->extents_moved = mirr_state->region_size *
		    numerator / seg->lv->vg->extent_size;

	return 1;
}
#endif

static void _destroy(const struct segment_type *segtype)
{
	dbg_free((void *) segtype);
}

static struct segtype_handler _mirrored_ops = {
	name:_name,
	display:_display,
	text_import_area_count:_text_import_area_count,
	text_import:_text_import,
	text_export:_text_export,
#ifdef DEVMAPPER_SUPPORT
	compose_target_line:_compose_target_line,
	target_percent:_target_percent,
#endif
	destroy:_destroy,
};

#ifdef MIRRORED_INTERNAL
struct segment_type *init_mirrored_segtype(struct cmd_context *cmd)
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
	segtype->ops = &_mirrored_ops;
	segtype->name = "mirror";
	segtype->private = NULL;
	segtype->flags = SEG_CAN_SPLIT | SEG_AREAS_MIRRORED;

	return segtype;
}
