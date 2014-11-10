/*
 * Copyright (C) 2011-2013 Red Hat, Inc. All rights reserved.
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
#include "segtype.h"
#include "display.h"
#include "text_export.h"
#include "text_import.h"
#include "config.h"
#include "str_list.h"
#include "targets.h"
#include "lvm-string.h"
#include "activate.h"
#include "metadata.h"
#include "lv_alloc.h"

static void _raid_display(const struct lv_segment *seg)
{
	unsigned s;

	for (s = 0; s < seg->area_count; ++s) {
		log_print("  Raid Data LV%2d", s);
		display_stripe(seg, s, "    ");
	}

	for (s = 0; s < seg->area_count; ++s)
		log_print("  Raid Metadata LV%2d\t%s", s, seg_metalv(seg, s)->name);

	log_print(" ");
}

static int _raid_text_import_area_count(const struct dm_config_node *sn,
					uint32_t *area_count)
{
	if (!dm_config_get_uint32(sn, "device_count", area_count)) {
		log_error("Couldn't read 'device_count' for "
			  "segment '%s'.", dm_config_parent_name(sn));
		return 0;
	}
	return 1;
}

static int _raid_text_import_areas(struct lv_segment *seg,
				   const struct dm_config_node *sn,
				   const struct dm_config_value *cv)
{
	unsigned int s;
	struct logical_volume *lv1;
	const char *seg_name = dm_config_parent_name(sn);

	if (!seg->area_count) {
		log_error("No areas found for segment %s", seg_name);
		return 0;
	}

	for (s = 0; cv && s < seg->area_count; s++, cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_error("Bad volume name in areas array for segment %s.", seg_name);
			return 0;
		}

		if (!cv->next) {
			log_error("Missing data device in areas array for segment %s.", seg_name);
			return 0;
		}

		/* Metadata device comes first */
		if (!(lv1 = find_lv(seg->lv->vg, cv->v.str))) {
			log_error("Couldn't find volume '%s' for segment '%s'.",
				  cv->v.str ? : "NULL", seg_name);
			return 0;
		}
		if (!set_lv_segment_area_lv(seg, s, lv1, 0, RAID_META))
				return_0;

		/* Data device comes second */
		cv = cv->next;
		if (!(lv1 = find_lv(seg->lv->vg, cv->v.str))) {
			log_error("Couldn't find volume '%s' for segment '%s'.",
				  cv->v.str ? : "NULL", seg_name);
			return 0;
		}
		if (!set_lv_segment_area_lv(seg, s, lv1, 0, RAID_IMAGE))
				return_0;
	}

	/*
	 * Check we read the correct number of RAID data/meta pairs.
	 */
	if (cv || (s < seg->area_count)) {
		log_error("Incorrect number of areas in area array "
			  "for segment '%s'.", seg_name);
		return 0;
	}

	return 1;
}

static int _raid_text_import(struct lv_segment *seg,
			     const struct dm_config_node *sn,
			     struct dm_hash_table *pv_hash)
{
	const struct dm_config_value *cv;

	if (dm_config_has_node(sn, "region_size")) {
		if (!dm_config_get_uint32(sn, "region_size", &seg->region_size)) {
			log_error("Couldn't read 'region_size' for "
				  "segment %s of logical volume %s.",
				  dm_config_parent_name(sn), seg->lv->name);
			return 0;
		}
	}
	if (dm_config_has_node(sn, "stripe_size")) {
		if (!dm_config_get_uint32(sn, "stripe_size", &seg->stripe_size)) {
			log_error("Couldn't read 'stripe_size' for "
				  "segment %s of logical volume %s.",
				  dm_config_parent_name(sn), seg->lv->name);
			return 0;
		}
	}
	if (dm_config_has_node(sn, "writebehind")) {
		if (!dm_config_get_uint32(sn, "writebehind", &seg->writebehind)) {
			log_error("Couldn't read 'writebehind' for "
				  "segment %s of logical volume %s.",
				  dm_config_parent_name(sn), seg->lv->name);
			return 0;
		}
	}
	if (dm_config_has_node(sn, "min_recovery_rate")) {
		if (!dm_config_get_uint32(sn, "min_recovery_rate",
					  &seg->min_recovery_rate)) {
			log_error("Couldn't read 'min_recovery_rate' for "
				  "segment %s of logical volume %s.",
				  dm_config_parent_name(sn), seg->lv->name);
			return 0;
		}
	}
	if (dm_config_has_node(sn, "max_recovery_rate")) {
		if (!dm_config_get_uint32(sn, "max_recovery_rate",
					  &seg->max_recovery_rate)) {
			log_error("Couldn't read 'max_recovery_rate' for "
				  "segment %s of logical volume %s.",
				  dm_config_parent_name(sn), seg->lv->name);
			return 0;
		}
	}
	if (!dm_config_get_list(sn, "raids", &cv)) {
		log_error("Couldn't find RAID array for "
			  "segment %s of logical volume %s.",
			  dm_config_parent_name(sn), seg->lv->name);
		return 0;
	}

	if (!_raid_text_import_areas(seg, sn, cv)) {
		log_error("Failed to import RAID images");
		return 0;
	}

	seg->status |= RAID;

	return 1;
}

static int _raid_text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "device_count = %u", seg->area_count);
	if (seg->region_size)
		outf(f, "region_size = %" PRIu32, seg->region_size);
	if (seg->stripe_size)
		outf(f, "stripe_size = %" PRIu32, seg->stripe_size);
	if (seg->writebehind)
		outf(f, "writebehind = %" PRIu32, seg->writebehind);
	if (seg->min_recovery_rate)
		outf(f, "min_recovery_rate = %" PRIu32, seg->min_recovery_rate);
	if (seg->max_recovery_rate)
		outf(f, "max_recovery_rate = %" PRIu32, seg->max_recovery_rate);

	return out_areas(f, seg, "raid");
}

static int _raid_add_target_line(struct dev_manager *dm __attribute__((unused)),
				 struct dm_pool *mem __attribute__((unused)),
				 struct cmd_context *cmd __attribute__((unused)),
				 void **target_state __attribute__((unused)),
				 struct lv_segment *seg,
				 const struct lv_activate_opts *laopts __attribute__((unused)),
				 struct dm_tree_node *node, uint64_t len,
				 uint32_t *pvmove_mirror_count __attribute__((unused)))
{
	uint32_t s;
	uint64_t flags = 0;
	uint64_t rebuilds = 0;
	uint64_t writemostly = 0;
	struct dm_tree_node_raid_params params;

	memset(&params, 0, sizeof(params));

	if (!seg->area_count) {
		log_error(INTERNAL_ERROR "_raid_add_target_line called "
			  "with no areas for %s.", seg->lv->name);
		return 0;
	}

	/*
	 * 64 device restriction imposed by kernel as well.  It is
	 * not strictly a userspace limitation.
	 */
	if (seg->area_count > 64) {
		log_error("Unable to handle more than 64 devices in a "
			  "single RAID array");
		return 0;
	}

	if (!seg->region_size) {
		log_error("Missing region size for mirror segment.");
		return 0;
	}

	for (s = 0; s < seg->area_count; s++)
		if (seg_lv(seg, s)->status & LV_REBUILD)
			rebuilds |= 1ULL << s;

	for (s = 0; s < seg->area_count; s++)
		if (seg_lv(seg, s)->status & LV_WRITEMOSTLY)
			writemostly |= 1ULL << s;

	if (mirror_in_sync())
		flags = DM_NOSYNC;

	params.raid_type = lvseg_name(seg);
	if (seg->segtype->parity_devs) {
		/* RAID 4/5/6 */
		params.mirrors = 1;
		params.stripes = seg->area_count - seg->segtype->parity_devs;
	} else if (strcmp(seg->segtype->name, SEG_TYPE_NAME_RAID10)) {
		/* RAID 10 only supports 2 mirrors now */
		params.mirrors = 2;
		params.stripes = seg->area_count / 2;
	} else {
		/* RAID 1 */
		params.mirrors = seg->area_count;
		params.stripes = 1;
		params.writebehind = seg->writebehind;
	}
	params.region_size = seg->region_size;
	params.stripe_size = seg->stripe_size;
	params.rebuilds = rebuilds;
	params.writemostly = writemostly;
	params.min_recovery_rate = seg->min_recovery_rate;
	params.max_recovery_rate = seg->max_recovery_rate;
	params.flags = flags;

	if (!dm_tree_node_add_raid_target_with_params(node, len, &params))
		return_0;

	return add_areas_line(dm, seg, node, 0u, seg->area_count);
}

static int _raid_target_status_compatible(const char *type)
{
	return (strstr(type, "raid") != NULL);
}

static void _raid_destroy(struct segment_type *segtype)
{
	dm_free((void *) segtype);
}

#ifdef DEVMAPPER_SUPPORT
static int _raid_target_percent(void **target_state,
				dm_percent_t *percent,
				struct dm_pool *mem,
				struct cmd_context *cmd,
				struct lv_segment *seg, char *params,
				uint64_t *total_numerator,
				uint64_t *total_denominator)
{
	int i;
	uint64_t numerator, denominator;
	char *pos = params;
	/*
	 * Status line:
	 *    <raid_type> <#devs> <status_chars> <synced>/<total>
	 * Example:
	 *    raid1 2 AA 1024000/1024000
	 */
	for (i = 0; i < 3; i++) {
		pos = strstr(pos, " ");
		if (pos)
			pos++;
		else
			break;
	}
	if (!pos || (sscanf(pos, "%" PRIu64 "/%" PRIu64 "%n",
			    &numerator, &denominator, &i) != 2)) {
		log_error("Failed to parse %s status fraction: %s",
			  (seg) ? seg->segtype->name : "segment", params);
		return 0;
	}

	*total_numerator += numerator;
	*total_denominator += denominator;

	if (seg)
		seg->extents_copied = seg->area_len * numerator / denominator;

	*percent = dm_make_percent(numerator, denominator);

	return 1;
}

static int _raid_target_present(struct cmd_context *cmd,
				const struct lv_segment *seg __attribute__((unused)),
				unsigned *attributes)
{
	/* List of features with their kernel target version */
	static const struct feature {
		uint32_t maj;
		uint32_t min;
		unsigned raid_feature;
		const char *feature;
	} _features[] = {
		{ 1, 3, RAID_FEATURE_RAID10, SEG_TYPE_NAME_RAID10 },
	};

	static int _raid_checked = 0;
	static int _raid_present = 0;
	static int _raid_attrs = 0;
	uint32_t maj, min, patchlevel;
	unsigned i;

	if (!_raid_checked) {
		_raid_present = target_present(cmd, "raid", 1);

		if (!target_version("raid", &maj, &min, &patchlevel)) {
			log_error("Cannot read target version of RAID kernel module.");
			return 0;
		}

		for (i = 0; i < DM_ARRAY_SIZE(_features); ++i)
			if ((maj > _features[i].maj) ||
			    (maj == _features[i].maj && min >= _features[i].min))
				_raid_attrs |= _features[i].raid_feature;
			else
				log_very_verbose("Target raid does not support %s.",
						 _features[i].feature);

		_raid_checked = 1;
	}

	if (attributes)
		*attributes = _raid_attrs;

	return _raid_present;
}

static int _raid_modules_needed(struct dm_pool *mem,
				const struct lv_segment *seg __attribute__((unused)),
				struct dm_list *modules)
{
	if (!str_list_add(mem, modules, "raid")) {
		log_error("raid module string list allocation failed");
		return 0;
	}

	return 1;
}

#  ifdef DMEVENTD
static const char *_get_raid_dso_path(struct cmd_context *cmd)
{
	const char *config_str = find_config_tree_str(cmd, dmeventd_raid_library_CFG, NULL);
	return get_monitor_dso_path(cmd, config_str);
}

static int _raid_target_monitored(struct lv_segment *seg, int *pending)
{
	struct cmd_context *cmd = seg->lv->vg->cmd;
	const char *dso_path = _get_raid_dso_path(cmd);

	return target_registered_with_dmeventd(cmd, dso_path, seg->lv, pending);
}

static int _raid_set_events(struct lv_segment *seg, int evmask, int set)
{
	struct cmd_context *cmd = seg->lv->vg->cmd;
	const char *dso_path = _get_raid_dso_path(cmd);

	return target_register_events(cmd, dso_path, seg->lv, evmask, set, 0);
}

static int _raid_target_monitor_events(struct lv_segment *seg, int events)
{
	return _raid_set_events(seg, events, 1);
}

static int _raid_target_unmonitor_events(struct lv_segment *seg, int events)
{
	return _raid_set_events(seg, events, 0);
}
#  endif /* DMEVENTD */
#endif /* DEVMAPPER_SUPPORT */

static struct segtype_handler _raid_ops = {
	.display = _raid_display,
	.text_import_area_count = _raid_text_import_area_count,
	.text_import = _raid_text_import,
	.text_export = _raid_text_export,
	.add_target_line = _raid_add_target_line,
	.target_status_compatible = _raid_target_status_compatible,
#ifdef DEVMAPPER_SUPPORT
	.target_percent = _raid_target_percent,
	.target_present = _raid_target_present,
	.modules_needed = _raid_modules_needed,
#  ifdef DMEVENTD
	.target_monitored = _raid_target_monitored,
	.target_monitor_events = _raid_target_monitor_events,
	.target_unmonitor_events = _raid_target_unmonitor_events,
#  endif        /* DMEVENTD */
#endif
	.destroy = _raid_destroy,
};

static const struct raid_type {
	const char name[12];
	unsigned parity;
	int extra_flags;
} _raid_types[] = {
	{ SEG_TYPE_NAME_RAID1,    0, SEG_AREAS_MIRRORED },
	{ SEG_TYPE_NAME_RAID10,   0, SEG_AREAS_MIRRORED },
	{ SEG_TYPE_NAME_RAID4,    1 },
	{ SEG_TYPE_NAME_RAID5,    1 },
	{ SEG_TYPE_NAME_RAID5_LA, 1 },
	{ SEG_TYPE_NAME_RAID5_LS, 1 },
	{ SEG_TYPE_NAME_RAID5_RA, 1 },
	{ SEG_TYPE_NAME_RAID5_RS, 1 },
	{ SEG_TYPE_NAME_RAID6,    2 },
	{ SEG_TYPE_NAME_RAID6_NC, 2 },
	{ SEG_TYPE_NAME_RAID6_NR, 2 },
	{ SEG_TYPE_NAME_RAID6_ZR, 2 }
};

static struct segment_type *_init_raid_segtype(struct cmd_context *cmd,
					       const struct raid_type *rt,
					       int monitored)
{
	struct segment_type *segtype = dm_zalloc(sizeof(*segtype));

	if (!segtype) {
		log_error("Failed to allocate memory for %s segtype",
			  rt->name);
		return NULL;
	}

	segtype->ops = &_raid_ops;
	segtype->name = rt->name;
	segtype->flags = SEG_RAID | SEG_ONLY_EXCLUSIVE | rt->extra_flags | monitored;
	segtype->parity_devs = rt->parity;

	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}

#ifdef RAID_INTERNAL /* Shared */
int init_raid_segtypes(struct cmd_context *cmd, struct segtype_library *seglib)
#else
int init_multiple_segtypes(struct cmd_context *cmd, struct segtype_library *seglib);

int init_multiple_segtypes(struct cmd_context *cmd, struct segtype_library *seglib)
#endif
{
	struct segment_type *segtype;
	unsigned i;
	int monitored = 0;

#ifdef DEVMAPPER_SUPPORT
#  ifdef DMEVENTD
	if (_get_raid_dso_path(cmd))
		monitored = SEG_MONITORED;
#  endif
#endif

	for (i = 0; i < DM_ARRAY_SIZE(_raid_types); ++i)
		if ((segtype = _init_raid_segtype(cmd, &_raid_types[i], monitored)) &&
		    !lvm_register_segtype(seglib, segtype))
			/* segtype is already destroyed */
			return_0;

	return 1;
}
