/*
 * Copyright (C) 2013-2016 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/metadata/segtype.h"
#include "lib/display/display.h"
#include "lib/format_text/text_export.h"
#include "lib/config/config.h"
#include "lib/datastruct/str_list.h"
#include "lib/misc/lvm-string.h"
#include "lib/activate/activate.h"
#include "lib/metadata/metadata.h"
#include "lib/metadata/lv_alloc.h"
#include "lib/config/defaults.h"

#define SEG_LOG_ERROR(t, p...) \
        log_error(t " segment %s of logical volume %s.", ## p,	\
                  dm_config_parent_name(sn), seg->lv->name), 0;

static void _integrity_display(const struct lv_segment *seg)
{
	/* TODO: lvdisplay segments */
}

static int _integrity_text_import(struct lv_segment *seg,
				   const struct dm_config_node *sn,
				   struct dm_hash_table *pv_hash __attribute__((unused)))
{
	struct integrity_settings *set;
	struct logical_volume *origin_lv = NULL;
	struct logical_volume *meta_lv = NULL;
	const char *origin_name = NULL;
	const char *meta_dev = NULL;
	const char *mode = NULL;
	const char *hash = NULL;

	memset(&seg->integrity_settings, 0, sizeof(struct integrity_settings));
	set = &seg->integrity_settings;

	/* origin always set */

	if (!dm_config_has_node(sn, "origin"))
		return SEG_LOG_ERROR("origin not specified in");

	if (!dm_config_get_str(sn, "origin", &origin_name))
		return SEG_LOG_ERROR("origin must be a string in");

	if (!(origin_lv = find_lv(seg->lv->vg, origin_name)))
		return SEG_LOG_ERROR("Unknown LV specified for integrity origin %s in", origin_name);

	if (!set_lv_segment_area_lv(seg, 0, origin_lv, 0, 0))
		return_0;

	/* data_sectors always set */

	if (!dm_config_get_uint64(sn, "data_sectors", &seg->integrity_data_sectors))
		return SEG_LOG_ERROR("integrity data_sectors must be set in");

	/* mode always set */

	if (!dm_config_get_str(sn, "mode", &mode))
		return SEG_LOG_ERROR("integrity mode must be set in");

	if (strlen(mode) > 7)
		return SEG_LOG_ERROR("integrity mode invalid in");

	strncpy(set->mode, mode, 7);

	/* tag_size always set */

	if (!dm_config_get_uint32(sn, "tag_size", &set->tag_size))
		return SEG_LOG_ERROR("integrity tag_size must be set in");

	/* block_size always set */

	if (!dm_config_get_uint32(sn, "block_size", &set->block_size))
		return SEG_LOG_ERROR("integrity block_size invalid in");

	/* internal_hash always set */

	if (!dm_config_get_str(sn, "internal_hash", &hash))
		return SEG_LOG_ERROR("integrity internal_hash must be set in");

	if (!(set->internal_hash = dm_pool_strdup(seg->lv->vg->vgmem, hash)))
		return SEG_LOG_ERROR("integrity internal_hash failed to be set in");

	/* meta_dev optional */

	if (dm_config_has_node(sn, "meta_dev")) {
		if (!dm_config_get_str(sn, "meta_dev", &meta_dev))
			return SEG_LOG_ERROR("meta_dev must be a string in");

		if (!(meta_lv = find_lv(seg->lv->vg, meta_dev)))
			return SEG_LOG_ERROR("Unknown logical volume %s specified for integrity in", meta_dev);
	}

	if (dm_config_has_node(sn, "recalculate")) {
		if (!dm_config_get_uint32(sn, "recalculate", &seg->integrity_recalculate))
			return SEG_LOG_ERROR("integrity recalculate error in");
	}

	/* the rest are optional */

	if (dm_config_has_node(sn, "journal_sectors")) {
		if (!dm_config_get_uint32(sn, "journal_sectors", &set->journal_sectors))
			return SEG_LOG_ERROR("Unknown integrity_setting in");
		set->journal_sectors_set = 1;
	}

	if (dm_config_has_node(sn, "interleave_sectors")) {
		if (!dm_config_get_uint32(sn, "interleave_sectors", &set->interleave_sectors))
			return SEG_LOG_ERROR("Unknown integrity_setting in");
		set->interleave_sectors_set = 1;
	}

	if (dm_config_has_node(sn, "buffer_sectors")) {
		if (!dm_config_get_uint32(sn, "buffer_sectors", &set->buffer_sectors))
			return SEG_LOG_ERROR("Unknown integrity_setting in");
		set->buffer_sectors_set = 1;
	}

	if (dm_config_has_node(sn, "journal_watermark")) {
		if (!dm_config_get_uint32(sn, "journal_watermark", &set->journal_watermark))
			return SEG_LOG_ERROR("Unknown integrity_setting in");
		set->journal_watermark_set = 1;
	}

	if (dm_config_has_node(sn, "commit_time")) {
		if (!dm_config_get_uint32(sn, "commit_time", &set->commit_time))
			return SEG_LOG_ERROR("Unknown integrity_setting in");
		set->commit_time_set = 1;
	}

	if (dm_config_has_node(sn, "bitmap_flush_interval")) {
		if (!dm_config_get_uint32(sn, "bitmap_flush_interval", &set->bitmap_flush_interval))
			return SEG_LOG_ERROR("Unknown integrity_setting in");
		set->bitmap_flush_interval_set = 1;
	}

	if (dm_config_has_node(sn, "sectors_per_bit")) {
		if (!dm_config_get_uint64(sn, "sectors_per_bit", &set->sectors_per_bit))
			return SEG_LOG_ERROR("Unknown integrity_setting in");
		set->sectors_per_bit_set = 1;
	}

	seg->origin = origin_lv;
	seg->integrity_meta_dev = meta_lv;
	seg->lv->status |= INTEGRITY;

	if (meta_lv)
		meta_lv->status |= INTEGRITY_METADATA;

	if (meta_lv && !add_seg_to_segs_using_this_lv(meta_lv, seg))
		return_0;

	return 1;
}

static int _integrity_text_import_area_count(const struct dm_config_node *sn,
					      uint32_t *area_count)
{
	*area_count = 1;

	return 1;
}

static int _integrity_text_export(const struct lv_segment *seg,
				   struct formatter *f)
{
	const struct integrity_settings *set = &seg->integrity_settings;

	outf(f, "origin = \"%s\"", seg_lv(seg, 0)->name);
	outf(f, "data_sectors = %llu", (unsigned long long)seg->integrity_data_sectors);

	outf(f, "mode = \"%s\"", set->mode);
	outf(f, "tag_size = %u", set->tag_size);
	outf(f, "block_size = %u", set->block_size);
	outf(f, "internal_hash = \"%s\"", set->internal_hash);

	if (seg->integrity_meta_dev)
		outf(f, "meta_dev = \"%s\"", seg->integrity_meta_dev->name);

	if (seg->integrity_recalculate)
		outf(f, "recalculate = %u", seg->integrity_recalculate);

	if (set->journal_sectors_set)
		outf(f, "journal_sectors = %u", set->journal_sectors);

	if (set->interleave_sectors_set)
		outf(f, "interleave_sectors = %u", set->interleave_sectors);

	if (set->buffer_sectors_set)
		outf(f, "buffer_sectors = %u", set->buffer_sectors);

	if (set->journal_watermark_set)
		outf(f, "journal_watermark = %u", set->journal_watermark);

	if (set->commit_time_set)
		outf(f, "commit_time = %u", set->commit_time);

	if (set->bitmap_flush_interval)
		outf(f, "bitmap_flush_interval = %u", set->bitmap_flush_interval);

	if (set->sectors_per_bit)
		outf(f, "sectors_per_bit = %llu", (unsigned long long)set->sectors_per_bit);

	return 1;
}

static void _destroy(struct segment_type *segtype)
{
	free((void *) segtype);
}

#ifdef DEVMAPPER_SUPPORT

static int _target_present(struct cmd_context *cmd,
			   const struct lv_segment *seg __attribute__((unused)),
			   unsigned *attributes __attribute__((unused)))
{
	static int _integrity_checked = 0;
	static int _integrity_present = 0;
	uint32_t maj, min, patchlevel;

	if (!activation())
		return 0;

	if (!_integrity_checked) {
		_integrity_checked = 1;
		if (!(_integrity_present = target_present_version(cmd, TARGET_NAME_INTEGRITY, 1,
								  &maj, &min, &patchlevel)))
			return 0;

		if (maj < 1 || min < 6) {
			log_error("Integrity target version older than minimum 1.6.0");
			return 0;
		}
	}

	return _integrity_present;
}

static int _modules_needed(struct dm_pool *mem,
			   const struct lv_segment *seg __attribute__((unused)),
			   struct dm_list *modules)
{
	if (!str_list_add(mem, modules, MODULE_NAME_INTEGRITY)) {
		log_error("String list allocation failed for integrity module.");
		return 0;
	}

	return 1;
}
#endif /* DEVMAPPER_SUPPORT */

#ifdef DEVMAPPER_SUPPORT
static int _integrity_add_target_line(struct dev_manager *dm,
				 struct dm_pool *mem,
				 struct cmd_context *cmd __attribute__((unused)),
				 void **target_state __attribute__((unused)),
				 struct lv_segment *seg,
				 const struct lv_activate_opts *laopts,
				 struct dm_tree_node *node, uint64_t len,
				 uint32_t *pvmove_mirror_count __attribute__((unused)))
{
	char *origin_uuid;
	char *meta_uuid = NULL;

	if (!seg_is_integrity(seg)) {
		log_error(INTERNAL_ERROR "Passed segment is not integrity.");
		return 0;
	}

	if (!(origin_uuid = build_dm_uuid(mem, seg_lv(seg, 0), NULL)))
		return_0;

	if (seg->integrity_meta_dev) {
		if (!(meta_uuid = build_dm_uuid(mem, seg->integrity_meta_dev, NULL)))
			return_0;
	}

	if (!seg->integrity_data_sectors) {
		log_error("_integrity_add_target_line zero size");
		return 0;
	}

	if (!dm_tree_node_add_integrity_target(node, seg->integrity_data_sectors,
					       origin_uuid, meta_uuid,
					       &seg->integrity_settings,
					       seg->integrity_recalculate))
		return_0;

	return 1;
}
#endif /* DEVMAPPER_SUPPORT */

static struct segtype_handler _integrity_ops = {
	.display = _integrity_display,
	.text_import = _integrity_text_import,
	.text_import_area_count = _integrity_text_import_area_count,
	.text_export = _integrity_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _integrity_add_target_line,
	.target_present = _target_present,
	.modules_needed = _modules_needed,
#endif
	.destroy = _destroy,
};

int init_integrity_segtypes(struct cmd_context *cmd,
			struct segtype_library *seglib)
{
	struct segment_type *segtype = zalloc(sizeof(*segtype));

	if (!segtype) {
		log_error("Failed to allocate memory for integrity segtype");
		return 0;
	}

	segtype->name = SEG_TYPE_NAME_INTEGRITY;
	segtype->flags = SEG_INTEGRITY;
	segtype->ops = &_integrity_ops;

	if (!lvm_register_segtype(seglib, segtype))
		return_0;
	log_very_verbose("Initialised segtype: %s", segtype->name);

	return 1;
}
