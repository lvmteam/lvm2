/*
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
#include "toolcontext.h"
#include "segtype.h"
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
	return seg->segtype->name;
}

static int _merge_segments(struct lv_segment *seg1, struct lv_segment *seg2)
{
	seg1->len += seg2->len;
	seg1->area_len += seg2->area_len;

	return 1;
}

#ifdef DEVMAPPER_SUPPORT
static int _add_target_line(struct dev_manager *dm, struct dm_pool *mem,
				struct config_tree *cft, void **target_state,
				struct lv_segment *seg,
				struct dm_tree_node *node, uint64_t len,
				uint32_t *pvmove_mirror_count)
{
	return dm_tree_node_add_error_target(node, len);
}

static int _target_present(void)
{
	static int checked = 0;
	static int present = 0;

	/* Reported truncated in older kernels */
	if (!checked &&
	    (target_present("error", 0) || target_present("erro", 0)))
		present = 1;

	checked = 1;
	return present;
}
#endif

static void _destroy(const struct segment_type *segtype)
{
	dm_free((void *) segtype);
}

static struct segtype_handler _error_ops = {
	name:_name,
	merge_segments:_merge_segments,
#ifdef DEVMAPPER_SUPPORT
	add_target_line:_add_target_line,
	target_present:_target_present,
#endif
	destroy:_destroy,
};

struct segment_type *init_error_segtype(struct cmd_context *cmd)
{
	struct segment_type *segtype = dm_malloc(sizeof(*segtype));

	if (!segtype)
		return_NULL;

	segtype->cmd = cmd;
	segtype->ops = &_error_ops;
	segtype->name = "error";
	segtype->private = NULL;
	segtype->flags = SEG_CAN_SPLIT | SEG_VIRTUAL | SEG_CANNOT_BE_ZEROED;

	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}
