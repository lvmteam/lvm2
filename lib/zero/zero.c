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
	return seg->segtype->name;
}

static int _merge_segments(struct lv_segment *seg1, struct lv_segment *seg2)
{
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
	/*   zero */

	*target = "zero";
	*params = '\0';

	return 1;
}

static int _target_present(void)
{
	static int checked = 0;
	static int present = 0;

	if (!checked)
		present = target_present("zero");

	checked = 1;
	return present;
}
#endif

static void _destroy(const struct segment_type *segtype)
{
	dbg_free((void *) segtype);
}

static struct segtype_handler _zero_ops = {
	name:_name,
	merge_segments:_merge_segments,
#ifdef DEVMAPPER_SUPPORT
	compose_target_line:_compose_target_line,
	target_present:_target_present,
#endif
	destroy:_destroy,
};

struct segment_type *init_zero_segtype(struct cmd_context *cmd)
{
	struct segment_type *segtype = dbg_malloc(sizeof(*segtype));

	if (!segtype) {
		stack;
		return NULL;
	}

	segtype->cmd = cmd;
	segtype->ops = &_zero_ops;
	segtype->name = "zero";
	segtype->private = NULL;
	segtype->flags = SEG_CAN_SPLIT | SEG_VIRTUAL;

	return segtype;
}
