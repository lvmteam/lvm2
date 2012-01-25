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
#include "defaults.h"

static const char *_raid_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
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
	uint64_t rebuilds = 0;

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
			rebuilds |= 1 << s;

	if (!dm_tree_node_add_raid_target(node, len, _raid_name(seg),
					  seg->region_size, seg->stripe_size,
					  rebuilds, 0))
		return_0;

	return add_areas_line(dm, seg, node, 0u, seg->area_count);
}

static int _raid_target_status_compatible(const char *type)
{
	return (strstr(type, "raid") != NULL);
}

static int _raid_target_percent(void **target_state,
				percent_t *percent,
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
			  seg->segtype->name, params);
		return 0;
	}

	*total_numerator += numerator;
	*total_denominator += denominator;

	if (seg)
		seg->extents_copied = seg->area_len * numerator / denominator;

	*percent = make_percent(numerator, denominator);

	return 1;
}


static int _raid_target_present(struct cmd_context *cmd,
				const struct lv_segment *seg __attribute__((unused)),
				unsigned *attributes __attribute__((unused)))
{
	static int _raid_checked = 0;
	static int _raid_present = 0;

	if (!_raid_checked)
		_raid_present = target_present(cmd, "raid", 1);

	_raid_checked = 1;

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

static void _raid_destroy(struct segment_type *segtype)
{
	dm_free((void *) segtype);
}

#ifdef DEVMAPPER_SUPPORT
#ifdef DMEVENTD
static const char *_get_raid_dso_path(struct cmd_context *cmd)
{
	const char *config_str = find_config_tree_str(cmd, "dmeventd/raid_library",
						      DEFAULT_DMEVENTD_RAID_LIB);
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
#endif /* DEVMAPPER_SUPPORT */
#endif /* DMEVENTD */
static struct segtype_handler _raid_ops = {
	.name = _raid_name,
	.text_import_area_count = _raid_text_import_area_count,
	.text_import = _raid_text_import,
	.text_export = _raid_text_export,
	.add_target_line = _raid_add_target_line,
	.target_status_compatible = _raid_target_status_compatible,
#ifdef DEVMAPPER_SUPPORT
	.target_percent = _raid_target_percent,
	.target_present = _raid_target_present,
#  ifdef DMEVENTD
	.target_monitored = _raid_target_monitored,
	.target_monitor_events = _raid_target_monitor_events,
	.target_unmonitor_events = _raid_target_unmonitor_events,
#  endif        /* DMEVENTD */
#endif
	.modules_needed = _raid_modules_needed,
	.destroy = _raid_destroy,
};

static struct segment_type *_init_raid_segtype(struct cmd_context *cmd,
					       const char *raid_type)
{
	struct segment_type *segtype = dm_zalloc(sizeof(*segtype));

	if (!segtype) {
		log_error("Failed to allocate memory for %s segtype",
			  raid_type);
		return NULL;
	}
	segtype->cmd = cmd;

	segtype->flags = SEG_RAID;
#ifdef DEVMAPPER_SUPPORT
#ifdef DMEVENTD
	if (_get_raid_dso_path(cmd))
		segtype->flags |= SEG_MONITORED;
#endif
#endif
	segtype->parity_devs = strstr(raid_type, "raid6") ? 2 : 1;

	segtype->ops = &_raid_ops;
	segtype->name = raid_type;

	segtype->private = NULL;

	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}

static struct segment_type *_init_raid1_segtype(struct cmd_context *cmd)
{
	struct segment_type *segtype;

	segtype = _init_raid_segtype(cmd, "raid1");
	if (!segtype)
		return NULL;

	segtype->flags |= SEG_AREAS_MIRRORED;
	segtype->parity_devs = 0;

	return segtype;
}

static struct segment_type *_init_raid4_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid4");
}

static struct segment_type *_init_raid5_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid5");
}

static struct segment_type *_init_raid5_la_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid5_la");
}

static struct segment_type *_init_raid5_ra_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid5_ra");
}

static struct segment_type *_init_raid5_ls_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid5_ls");
}

static struct segment_type *_init_raid5_rs_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid5_rs");
}

static struct segment_type *_init_raid6_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid6");
}

static struct segment_type *_init_raid6_zr_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid6_zr");
}

static struct segment_type *_init_raid6_nr_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid6_nr");
}

static struct segment_type *_init_raid6_nc_segtype(struct cmd_context *cmd)
{
	return _init_raid_segtype(cmd, "raid6_nc");
}

#ifdef RAID_INTERNAL /* Shared */
int init_raid_segtypes(struct cmd_context *cmd, struct segtype_library *seglib)
#else
int init_multiple_segtypes(struct cmd_context *cmd, struct segtype_library *seglib);

int init_multiple_segtypes(struct cmd_context *cmd, struct segtype_library *seglib)
#endif
{
	struct segment_type *segtype;
	unsigned i = 0;
	struct segment_type *(*raid_segtype_fn[])(struct cmd_context *) =  {
		_init_raid1_segtype,
		_init_raid4_segtype,
		_init_raid5_segtype,
		_init_raid5_la_segtype,
		_init_raid5_ra_segtype,
		_init_raid5_ls_segtype,
		_init_raid5_rs_segtype,
		_init_raid6_segtype,
		_init_raid6_zr_segtype,
		_init_raid6_nr_segtype,
		_init_raid6_nc_segtype,
		NULL,
	};

	do {
		if ((segtype = raid_segtype_fn[i](cmd)) &&
		    !lvm_register_segtype(seglib, segtype)) {
			dm_free(segtype);
			return_0;
		}
	} while (raid_segtype_fn[++i]);

	return 1;
}
