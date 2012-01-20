/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2008 Red Hat, Inc. All rights reserved.
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
#include "config.h"
#include "activate.h"
#include "str_list.h"
#include "defaults.h"

static const char *_snap_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static const char *_snap_target_name(const struct lv_segment *seg,
				     const struct lv_activate_opts *laopts)
{
	if (!laopts->no_merging && (seg->status & MERGING))
		return "snapshot-merge";

	return _snap_name(seg);
}

static int _snap_text_import(struct lv_segment *seg, const struct dm_config_node *sn,
			struct dm_hash_table *pv_hash __attribute__((unused)))
{
	uint32_t chunk_size;
	const char *org_name, *cow_name;
	struct logical_volume *org, *cow;
	int old_suppress, merge = 0;

	if (!dm_config_get_uint32(sn, "chunk_size", &chunk_size)) {
		log_error("Couldn't read chunk size for snapshot.");
		return 0;
	}

	old_suppress = log_suppress(1);

	if ((cow_name = dm_config_find_str(sn, "merging_store", NULL))) {
		if (dm_config_find_str(sn, "cow_store", NULL)) {
			log_suppress(old_suppress);
			log_error("Both snapshot cow and merging storage were specified.");
			return 0;
		}
		merge = 1;
	}
	else if (!(cow_name = dm_config_find_str(sn, "cow_store", NULL))) {
		log_suppress(old_suppress);
		log_error("Snapshot cow storage not specified.");
		return 0;
	}

	if (!(org_name = dm_config_find_str(sn, "origin", NULL))) {
		log_suppress(old_suppress);
		log_error("Snapshot origin not specified.");
		return 0;
	}

	log_suppress(old_suppress);

	if (!(cow = find_lv(seg->lv->vg, cow_name))) {
		log_error("Unknown logical volume specified for "
			  "snapshot cow store.");
		return 0;
	}

	if (!(org = find_lv(seg->lv->vg, org_name))) {
		log_error("Unknown logical volume specified for "
			  "snapshot origin.");
		return 0;
	}

	init_snapshot_seg(seg, org, cow, chunk_size, merge);

	return 1;
}

static int _snap_text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "chunk_size = %u", seg->chunk_size);
	outf(f, "origin = \"%s\"", seg->origin->name);
	if (!(seg->status & MERGING))
		outf(f, "cow_store = \"%s\"", seg->cow->name);
	else
		outf(f, "merging_store = \"%s\"", seg->cow->name);

	return 1;
}

static int _snap_target_status_compatible(const char *type)
{
	return (strcmp(type, "snapshot-merge") == 0);
}

#ifdef DEVMAPPER_SUPPORT
static int _snap_target_percent(void **target_state __attribute__((unused)),
				percent_t *percent,
				struct dm_pool *mem __attribute__((unused)),
				struct cmd_context *cmd __attribute__((unused)),
				struct lv_segment *seg __attribute__((unused)),
				char *params, uint64_t *total_numerator,
				uint64_t *total_denominator)
{
	uint64_t total_sectors, sectors_allocated, metadata_sectors;
	int r;

	/*
	 * snapshot target's percent format:
	 * <= 1.7.0: <sectors_allocated>/<total_sectors>
	 * >= 1.8.0: <sectors_allocated>/<total_sectors> <metadata_sectors>
	 */
	r = sscanf(params, "%" PRIu64 "/%" PRIu64 " %" PRIu64,
		   &sectors_allocated, &total_sectors, &metadata_sectors);
	if (r == 2 || r == 3) {
		*total_numerator += sectors_allocated;
		*total_denominator += total_sectors;
		if (r == 3 && sectors_allocated == metadata_sectors)
			*percent = PERCENT_0;
		else if (sectors_allocated == total_sectors)
			*percent = PERCENT_100;
		else
			*percent = make_percent(*total_numerator, *total_denominator);
	}
	else if (!strcmp(params, "Invalid"))
		*percent = PERCENT_INVALID;
	else if (!strcmp(params, "Merge failed"))
		*percent = PERCENT_MERGE_FAILED;
	else
		return 0;

	return 1;
}

static int _snap_target_present(struct cmd_context *cmd,
				const struct lv_segment *seg,
				unsigned *attributes __attribute__((unused)))
{
	static int _snap_checked = 0;
	static int _snap_merge_checked = 0;
	static int _snap_present = 0;
	static int _snap_merge_present = 0;

	if (!_snap_checked) {
		_snap_present = target_present(cmd, "snapshot", 1) &&
		    target_present(cmd, "snapshot-origin", 0);
		_snap_checked = 1;
	}

	if (!_snap_merge_checked && seg && (seg->status & MERGING)) {
		_snap_merge_present = target_present(cmd, "snapshot-merge", 0);
		_snap_merge_checked = 1;
		return _snap_present && _snap_merge_present;
	}

	return _snap_present;
}

#ifdef DMEVENTD

static const char *_get_snapshot_dso_path(struct cmd_context *cmd)
{
	return get_monitor_dso_path(cmd, find_config_tree_str(cmd, "dmeventd/snapshot_library",
							      DEFAULT_DMEVENTD_SNAPSHOT_LIB));
}

/* FIXME Cache this */
static int _target_registered(struct lv_segment *seg, int *pending)
{
	return target_registered_with_dmeventd(seg->lv->vg->cmd, _get_snapshot_dso_path(seg->lv->vg->cmd),
					       seg->cow, pending);
}

/* FIXME This gets run while suspended and performs banned operations. */
static int _target_set_events(struct lv_segment *seg, int evmask, int set)
{
	/* FIXME Make timeout (10) configurable */
	return target_register_events(seg->lv->vg->cmd, _get_snapshot_dso_path(seg->lv->vg->cmd),
				      seg->cow, evmask, set, 10);
}

static int _target_register_events(struct lv_segment *seg,
				   int events)
{
	return _target_set_events(seg, events, 1);
}

static int _target_unregister_events(struct lv_segment *seg,
				     int events)
{
	return _target_set_events(seg, events, 0);
}

#endif /* DMEVENTD */
#endif

static int _snap_modules_needed(struct dm_pool *mem,
				const struct lv_segment *seg __attribute__((unused)),
				struct dm_list *modules)
{
	if (!str_list_add(mem, modules, "snapshot")) {
		log_error("snapshot string list allocation failed");
		return 0;
	}

	return 1;
}

static void _snap_destroy(struct segment_type *segtype)
{
	dm_free(segtype);
}

static struct segtype_handler _snapshot_ops = {
	.name = _snap_name,
	.target_name = _snap_target_name,
	.text_import = _snap_text_import,
	.text_export = _snap_text_export,
	.target_status_compatible = _snap_target_status_compatible,
#ifdef DEVMAPPER_SUPPORT
	.target_percent = _snap_target_percent,
	.target_present = _snap_target_present,
#  ifdef DMEVENTD
	.target_monitored = _target_registered,
	.target_monitor_events = _target_register_events,
	.target_unmonitor_events = _target_unregister_events,
#  endif	/* DMEVENTD */
#endif
	.modules_needed = _snap_modules_needed,
	.destroy = _snap_destroy,
};

#ifdef SNAPSHOT_INTERNAL
struct segment_type *init_snapshot_segtype(struct cmd_context *cmd)
#else				/* Shared */
struct segment_type *init_segtype(struct cmd_context *cmd);
struct segment_type *init_segtype(struct cmd_context *cmd)
#endif
{
	struct segment_type *segtype = dm_zalloc(sizeof(*segtype));

	if (!segtype)
		return_NULL;

	segtype->cmd = cmd;
	segtype->ops = &_snapshot_ops;
	segtype->name = "snapshot";
	segtype->private = NULL;
	segtype->flags = SEG_SNAPSHOT;

#ifdef DEVMAPPER_SUPPORT
#  ifdef DMEVENTD
	if (_get_snapshot_dso_path(cmd))
		segtype->flags |= SEG_MONITORED;
#  endif	/* DMEVENTD */
#endif
	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}
