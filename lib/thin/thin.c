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

/*
 * Macro used as return argument - returns 0.
 * return is left to be written in the function for better readability.
 */
#define SEG_LOG_ERROR(t, p...) \
	log_error(t " segment %s of logical volume %s.", ## p, \
		  config_parent_name(sn), seg->lv->name), 0;

static const char *_thin_pool_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static int _thin_pool_text_import(struct lv_segment *seg, const struct config_node *sn,
			struct dm_hash_table *pv_hash __attribute__((unused)))
{
	const struct config_node *cn;

	if (!(cn = find_config_node(sn, "data")) ||
	    !cn->v || cn->v->type != CFG_STRING)
		return SEG_LOG_ERROR("Thin pool data must be a string in");

	if (!(seg->data_lv = find_lv(seg->lv->vg, cn->v->v.str)))
		return SEG_LOG_ERROR("Unknown pool data %s in",
				     cn->v->v.str);

	if (!(cn = find_config_node(sn, "metadata")) ||
	    !cn->v || cn->v->type != CFG_STRING)
		return SEG_LOG_ERROR("Thin pool metadata must be a string in");

	if (!(seg->metadata_lv = find_lv(seg->lv->vg, cn->v->v.str)))
		return SEG_LOG_ERROR("Unknown pool metadata %s in",
				     cn->v->v.str);

	if (!get_config_uint64(sn, "transaction_id", &seg->transaction_id))
		return SEG_LOG_ERROR("Could not read transaction_id for");

	if (find_config_node(sn, "zero_new_blocks") &&
	    !get_config_uint32(sn, "zero_new_blocks", &seg->zero_new_blocks))
		return SEG_LOG_ERROR("Could not read zero_new_blocks for");

	return 1;
}

static int _thin_pool_text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "data = \"%s\"", seg->data_lv->name);
	outf(f, "metadata = \"%s\"", seg->metadata_lv->name);
	outf(f, "transaction_id = %" PRIu64, seg->transaction_id);
	if (seg->zero_new_blocks)
		outf(f, "zero_new_blocks = 1");

	return 1;
}

static const char *_thin_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static int _thin_text_import(struct lv_segment *seg, const struct config_node *sn,
			struct dm_hash_table *pv_hash __attribute__((unused)))
{
	const struct config_node *cn;

	if (!(cn = find_config_node(sn, "thin_pool")) ||
	    !cn->v || cn->v->type != CFG_STRING)
		return SEG_LOG_ERROR("Thin pool must be a string in");

	if (!(seg->thin_pool_lv = find_lv(seg->lv->vg, cn->v->v.str)))
		return SEG_LOG_ERROR("Unknown thin pool %s in",
				     cn->v->v.str);

	if ((cn = find_config_node(sn, "origin"))) {
                if (!cn->v || cn->v->type != CFG_STRING)
			return SEG_LOG_ERROR("Thin pool origin must be a string in");

		if (!(seg->origin = find_lv(seg->lv->vg, cn->v->v.str)))
			return SEG_LOG_ERROR("Unknown origin %s in",
					     cn->v->v.str);
	}

	if (!get_config_uint64(sn, "device_id", &seg->device_id))
		return SEG_LOG_ERROR("Could not read device_id for");

	return 1;
}

static int _thin_text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "thin_pool = \"%s\"", seg->thin_pool_lv->name);
	outf(f, "device_id = %" PRIu64, seg->device_id);

	if (seg->origin)
		outf(f, "origin = \"%s\"", seg->origin->name);

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
		_present = target_present(cmd, THIN_MODULE, 1);
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

static struct segtype_handler _thin_pool_ops = {
	.name = _thin_pool_name,
	.text_import = _thin_pool_text_import,
	.text_export = _thin_pool_text_export,
	.modules_needed = _thin_modules_needed,
	.destroy = _thin_destroy,
};

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
int init_thin_segtypes(struct cmd_context *cmd, struct segtype_library *seglib)
#else /* Shared */
int init_multiple_segtypes(struct cmd_context *cmd, struct segtype_library *seglib);
int init_multiple_segtypes(struct cmd_context *cmd, struct segtype_library *seglib)
#endif
{
	static const struct {
		struct segtype_handler *ops;
		const char name[16];
		uint32_t flags;
	} reg_segtypes[] = {
		{ &_thin_pool_ops, "thin_pool", SEG_THIN_POOL },
		{ &_thin_ops, "thin", SEG_THIN_VOLUME }
	};

	struct segment_type *segtype;
	unsigned i;

	for (i = 0; i < sizeof(reg_segtypes)/sizeof(reg_segtypes[0]); ++i) {
		segtype = dm_zalloc(sizeof(*segtype));

		if (!segtype) {
			log_error("Failed to allocate memory for %s segtype",
				  reg_segtypes[i].name);
			return 0;
		}

		segtype->ops = reg_segtypes[i].ops;
		segtype->name = reg_segtypes[i].name;
		segtype->flags = reg_segtypes[i].flags;

#ifdef DEVMAPPER_SUPPORT
#  ifdef DMEVENTD
// FIXME		if (_get_thin_dso_path(cmd))
// FIXME			segtype->flags |= SEG_MONITORED;
#  endif	/* DMEVENTD */
#endif
		if (!lvm_register_segtype(seglib, segtype))
			return_0;

		log_very_verbose("Initialised segtype: %s", segtype->name);
	}

	return 1;
}
