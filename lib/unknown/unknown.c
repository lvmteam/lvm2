/*
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "toolcontext.h"
#include "segtype.h"
#include "text_export.h"
#include "config.h"

static int _unknown_text_import(struct lv_segment *seg, const struct dm_config_node *sn,
				struct dm_hash_table *pv_hash)
{
	struct dm_config_node *new, *last = NULL, *head = NULL;
	const struct dm_config_node *current;
	log_verbose("importing unknown segment");
	for (current = sn; current != NULL; current = current->sib) {
		if (!strcmp(current->key, "type") || !strcmp(current->key, "start_extent") ||
		    !strcmp(current->key, "tags") || !strcmp(current->key, "extent_count"))
			continue;
		new = dm_config_clone_node_with_mem(seg->lv->vg->vgmem, current, 0);
		if (!new)
			return_0;
		if (last)
			last->sib = new;
		if (!head)
			head = new;
		last = new;
	}
	seg->segtype_private = head;
	return 1;
}

static int _unknown_text_export(const struct lv_segment *seg, struct formatter *f)
{
	struct dm_config_node *cn = seg->segtype_private;
	return out_config_node(f, cn);
}

static void _unknown_destroy(struct segment_type *segtype)
{
	dm_free(segtype);
}

static struct segtype_handler _unknown_ops = {
	.text_import = _unknown_text_import,
	.text_export = _unknown_text_export,
	.destroy = _unknown_destroy,
};

struct segment_type *init_unknown_segtype(struct cmd_context *cmd, const char *name)
{
	struct segment_type *segtype = dm_zalloc(sizeof(*segtype));

	if (!segtype) {
		log_error("Failed to allocate memory for unknown segtype");
		return NULL;
	}

	segtype->ops = &_unknown_ops;
	segtype->name = dm_pool_strdup(cmd->libmem, name);
	segtype->flags = SEG_UNKNOWN | SEG_VIRTUAL | SEG_CANNOT_BE_ZEROED;

	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}
