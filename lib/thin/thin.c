/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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
#include "metadata.h"
#include "segtype.h"
#include "text_export.h"
#include "text_import.h"
#include "config.h"
#include "activate.h"
#include "str_list.h"

#ifdef DMEVENTD
#  include "sharedlib.h"
#  include "libdevmapper-event.h"
#endif

/* Dm kernel module name for thin provisiong */
#define THIN_MODULE "thin-pool"


static const char *_thin_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static int _thin_text_import(struct lv_segment *seg, const struct config_node *sn,
			struct dm_hash_table *pv_hash __attribute__((unused)))
{
	return 1;
}

static int _thin_text_export(const struct lv_segment *seg, struct formatter *f)
{
	return 1;
}

#ifdef DEVMAPPER_SUPPORT
static int _thin_target_percent(void **target_state __attribute__((unused)),
				percent_t *percent,
				struct dm_pool *mem __attribute__((unused)),
				struct cmd_context *cmd __attribute__((unused)),
				struct lv_segment *seg __attribute__((unused)),
				char *params, uint64_t *total_numerator,
				uint64_t *total_denominator)
{
	return 1;
}

static int _thin_target_present(struct cmd_context *cmd,
				const struct lv_segment *seg,
				unsigned *attributes __attribute__((unused)))
{
	static int _checked = 0;
	static int _present = 0;

	if (!_checked) {
		_present = target_present(cmd, "thin-pool", 1);
		_checked = 1;
	}

	return _present;
}

#endif

static int _thin_modules_needed(struct dm_pool *mem,
				const struct lv_segment *seg __attribute__((unused)),
				struct dm_list *modules)
{
	if (!str_list_add(mem, modules, THIN_MODULE)) {
		log_error("thin string list allocation failed");
		return 0;
	}

	return 1;
}

static void _thin_destroy(struct segment_type *segtype)
{
	dm_free(segtype);
}

static struct segtype_handler _thin_ops = {
	.name = _thin_name,
	.text_import = _thin_text_import,
	.text_export = _thin_text_export,
#ifdef DEVMAPPER_SUPPORT
	.target_percent = _thin_target_percent,
	.target_present = _thin_target_present,
#endif
	.modules_needed = _thin_modules_needed,
	.destroy = _thin_destroy,
};

#ifdef THIN_INTERNAL
struct segment_type *init_thin_segtype(struct cmd_context *cmd)
#else				/* Shared */
struct segment_type *init_segtype(struct cmd_context *cmd);
struct segment_type *init_segtype(struct cmd_context *cmd)
#endif
{
	struct segment_type *segtype = dm_zalloc(sizeof(*segtype));

	if (!segtype)
		return_NULL;

	segtype->cmd = cmd;
	segtype->ops = &_thin_ops;
	segtype->name = "thin";
	segtype->private = NULL;
	segtype->flags = SEG_THIN;

#ifdef DEVMAPPER_SUPPORT
#  ifdef DMEVENTD
	if (_get_thin_dso_path(cmd))
		segtype->flags |= SEG_MONITORED;
#  endif	/* DMEVENTD */
#endif
	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}
