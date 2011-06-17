/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
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
#include "metadata.h"
#include "segtype.h"
#include "display.h"
#include "text_export.h"
#include "text_import.h"
#include "config.h"
#include "defaults.h"
#include "lvm-string.h"
#include "targets.h"
#include "activate.h"
#include "sharedlib.h"
#include "str_list.h"

#include <sys/utsname.h>

enum {
	MIRR_DISABLED,
	MIRR_RUNNING,
	MIRR_COMPLETED
};

struct mirror_state {
	uint32_t default_region_size;
};

static const char *_mirrored_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static void _mirrored_display(const struct lv_segment *seg)
{
	const char *size;
	uint32_t s;

	log_print("  Mirrors\t\t%u", seg->area_count);
	log_print("  Mirror size\t\t%u", seg->area_len);
	if (seg->log_lv)
		log_print("  Mirror log volume\t%s", seg->log_lv->name);

	if (seg->region_size) {
		size = display_size(seg->lv->vg->cmd,
				    (uint64_t) seg->region_size);
		log_print("  Mirror region size\t%s", size);
	}

	log_print("  Mirror original:");
	display_stripe(seg, 0, "    ");
	log_print("  Mirror destinations:");
	for (s = 1; s < seg->area_count; s++)
		display_stripe(seg, s, "    ");
	log_print(" ");
}

static int _mirrored_text_import_area_count(const struct config_node *sn, uint32_t *area_count)
{
	if (!get_config_uint32(sn, "mirror_count", area_count)) {
		log_error("Couldn't read 'mirror_count' for "
			  "segment '%s'.", config_parent_name(sn));
		return 0;
	}

	return 1;
}

static int _mirrored_text_import(struct lv_segment *seg, const struct config_node *sn,
			struct dm_hash_table *pv_hash)
{
	const struct config_node *cn;
	const char *logname = NULL;

	if (find_config_node(sn, "extents_moved")) {
		if (get_config_uint32(sn, "extents_moved",
				      &seg->extents_copied))
			seg->status |= PVMOVE;
		else {
			log_error("Couldn't read 'extents_moved' for "
				  "segment %s of logical volume %s.",
				  config_parent_name(sn), seg->lv->name);
			return 0;
		}
	}

	if (find_config_node(sn, "region_size")) {
		if (!get_config_uint32(sn, "region_size",
				      &seg->region_size)) {
			log_error("Couldn't read 'region_size' for "
				  "segment %s of logical volume %s.",
				  config_parent_name(sn), seg->lv->name);
			return 0;
		}
	}

	if ((cn = find_config_node(sn, "mirror_log"))) {
		if (!cn->v || !cn->v->v.str) {
			log_error("Mirror log type must be a string.");
			return 0;
		}
		logname = cn->v->v.str;
		if (!(seg->log_lv = find_lv(seg->lv->vg, logname))) {
			log_error("Unrecognised mirror log in "
				  "segment %s of logical volume %s.",
				  config_parent_name(sn), seg->lv->name);
			return 0;
		}
		seg->log_lv->status |= MIRROR_LOG;
	}

	if (logname && !seg->region_size) {
		log_error("Missing region size for mirror log for "
			  "segment %s of logical volume %s.",
			  config_parent_name(sn), seg->lv->name);
		return 0;
	}

	if (!(cn = find_config_node(sn, "mirrors"))) {
		log_error("Couldn't find mirrors array for "
			  "segment %s of logical volume %s.",
			  config_parent_name(sn), seg->lv->name);
		return 0;
	}

	return text_import_areas(seg, sn, cn, pv_hash, MIRROR_IMAGE);
}

static int _mirrored_text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "mirror_count = %u", seg->area_count);
	if (seg->status & PVMOVE)
		outsize(f, (uint64_t) seg->extents_copied * seg->lv->vg->extent_size,
			"extents_moved = %" PRIu32, seg->extents_copied);
	if (seg->log_lv)
		outf(f, "mirror_log = \"%s\"", seg->log_lv->name);
	if (seg->region_size)
		outf(f, "region_size = %" PRIu32, seg->region_size);

	return out_areas(f, seg, "mirror");
}

#ifdef DEVMAPPER_SUPPORT
static int _block_on_error_available = 0;
static unsigned _mirror_attributes = 0;

static struct mirror_state *_mirrored_init_target(struct dm_pool *mem,
					 struct cmd_context *cmd)
{
	struct mirror_state *mirr_state;

	if (!(mirr_state = dm_pool_alloc(mem, sizeof(*mirr_state)))) {
		log_error("struct mirr_state allocation failed");
		return NULL;
	}

	mirr_state->default_region_size = 2 *
	    find_config_tree_int(cmd,
			    "activation/mirror_region_size",
			    DEFAULT_MIRROR_REGION_SIZE);

	return mirr_state;
}

static int _mirrored_target_percent(void **target_state,
				    percent_t *percent,
				    struct dm_pool *mem,
				    struct cmd_context *cmd,
				    struct lv_segment *seg, char *params,
				    uint64_t *total_numerator,
				    uint64_t *total_denominator)
{
	uint64_t numerator, denominator;
	unsigned mirror_count, m;
	int used;
	char *pos = params;

	if (!*target_state)
		*target_state = _mirrored_init_target(mem, cmd);

	/* Status line: <#mirrors> (maj:min)+ <synced>/<total_regions> */
	log_debug("Mirror status: %s", params);

	if (sscanf(pos, "%u %n", &mirror_count, &used) != 1) {
		log_error("Failure parsing mirror status mirror count: %s",
			  params);
		return 0;
	}
	pos += used;

	for (m = 0; m < mirror_count; m++) {
		if (sscanf(pos, "%*x:%*x %n", &used) != 0) {
			log_error("Failure parsing mirror status devices: %s",
				  params);
			return 0;
		}
		pos += used;
	}

	if (sscanf(pos, "%" PRIu64 "/%" PRIu64 "%n", &numerator, &denominator,
		   &used) != 2) {
		log_error("Failure parsing mirror status fraction: %s", params);
		return 0;
	}
	pos += used;

	*total_numerator += numerator;
	*total_denominator += denominator;

	if (seg)
		seg->extents_copied = seg->area_len * numerator / denominator;

        *percent = make_percent(numerator, denominator);

	return 1;
}

static int _mirrored_transient_status(struct lv_segment *seg, char *params)
{
	int i, j;
	struct logical_volume *lv = seg->lv;
	struct lvinfo info;
	char *p = NULL;
	char **args, **log_args;
	struct logical_volume **images;
	struct logical_volume *log;
	int num_devs, log_argc;
	int failed = 0;
	char *status;

	log_very_verbose("Mirrored transient status: \"%s\"", params);

	/* number of devices */
	if (!dm_split_words(params, 1, 0, &p))
		return_0;

	if (!(num_devs = atoi(p)))
		return_0;

	p += strlen(p) + 1;

	if (num_devs > DEFAULT_MIRROR_MAX_IMAGES) {
		log_error("Unexpectedly many (%d) mirror images in %s.",
			  num_devs, lv->name);
		return_0;
	}

	args = alloca((num_devs + 5) * sizeof(char *));
	images = alloca(num_devs * sizeof(struct logical_volume *));

	if (dm_split_words(p, num_devs + 4, 0, args) < num_devs + 4)
		return_0;

	log_argc = atoi(args[3 + num_devs]);
	log_args = alloca(log_argc * sizeof(char *));

	if (log_argc > 16) {
		log_error("Unexpectedly many (%d) log arguments in %s.",
			  log_argc, lv->name);
		return_0;
	}


	if (dm_split_words(args[3 + num_devs] + strlen(args[3 + num_devs]) + 1,
			   log_argc, 0, log_args) < log_argc)
		return_0;

	if (num_devs != seg->area_count) {
		log_error("Active mirror has a wrong number of mirror images!");
		log_error("Metadata says %d, kernel says %d.", seg->area_count, num_devs);
		return_0;
	}

	if (!strcmp(log_args[0], "disk")) {
		char buf[32];
		log = first_seg(lv)->log_lv;
		if (!lv_info(lv->vg->cmd, log, 0, &info, 0, 0)) {
			log_error("Check for existence of mirror log %s failed.",
				  log->name);
			return 0;
		}
		log_debug("Found mirror log at %d:%d", info.major, info.minor);
		sprintf(buf, "%d:%d", info.major, info.minor);
		if (strcmp(buf, log_args[1])) {
			log_error("Mirror log mismatch. Metadata says %s, kernel says %s.",
				  buf, log_args[1]);
			return_0;
		}
		log_very_verbose("Status of log (%s): %s", buf, log_args[2]);
		if (log_args[2][0] != 'A') {
			log->status |= PARTIAL_LV;
			++failed;
		}
	}

	for (i = 0; i < num_devs; ++i)
		images[i] = NULL;

	for (i = 0; i < seg->area_count; ++i) {
		char buf[32];
		if (!lv_info(lv->vg->cmd, seg_lv(seg, i), 0, &info, 0, 0)) {
			log_error("Check for existence of mirror image %s failed.",
				  seg_lv(seg, i)->name);
			return 0;
		}
		log_debug("Found mirror image at %d:%d", info.major, info.minor);
		sprintf(buf, "%d:%d", info.major, info.minor);
		for (j = 0; j < num_devs; ++j) {
			if (!strcmp(buf, args[j])) {
			    log_debug("Match: metadata image %d matches kernel image %d", i, j);
			    images[j] = seg_lv(seg, i);
			}
		}
	}

	status = args[2 + num_devs];

	for (i = 0; i < num_devs; ++i) {
		if (!images[i]) {
			log_error("Failed to find image %d (%s).", i, args[i]);
			return_0;
		}
		log_very_verbose("Status of image %d: %c", i, status[i]);
		if (status[i] != 'A') {
			images[i]->status |= PARTIAL_LV;
			++failed;
		}
	}

	/* update PARTIAL_LV flags across the VG */
	if (failed)
		vg_mark_partial_lvs(lv->vg, 0);

	return 1;
}

static int _add_log(struct dm_pool *mem, struct lv_segment *seg,
		    const struct lv_activate_opts *laopts,
		    struct dm_tree_node *node, uint32_t area_count, uint32_t region_size)
{
	unsigned clustered = 0;
	char *log_dlid = NULL;
	uint32_t log_flags = 0;

	/*
	 * Use clustered mirror log for non-exclusive activation
	 * in clustered VG.
	 */
	if (!laopts->exclusive && vg_is_clustered(seg->lv->vg))
		clustered = 1;

	if (seg->log_lv) {
		/* If disk log, use its UUID */
		if (!(log_dlid = build_dm_uuid(mem, seg->log_lv->lvid.s, NULL))) {
			log_error("Failed to build uuid for log LV %s.",
				  seg->log_lv->name);
			return 0;
		}
	} else {
		/* If core log, use mirror's UUID and set DM_CORELOG flag */
		if (!(log_dlid = build_dm_uuid(mem, seg->lv->lvid.s, NULL))) {
			log_error("Failed to build uuid for mirror LV %s.",
				  seg->lv->name);
			return 0;
		}
		log_flags |= DM_CORELOG;
	}

	if (mirror_in_sync() && !(seg->status & PVMOVE))
		log_flags |= DM_NOSYNC;

	if (_block_on_error_available && !(seg->status & PVMOVE))
		log_flags |= DM_BLOCK_ON_ERROR;

	return dm_tree_node_add_mirror_target_log(node, region_size, clustered, log_dlid, area_count, log_flags);
}

static int _mirrored_add_target_line(struct dev_manager *dm, struct dm_pool *mem,
				     struct cmd_context *cmd, void **target_state,
				     struct lv_segment *seg,
				     const struct lv_activate_opts *laopts,
				     struct dm_tree_node *node, uint64_t len,
				     uint32_t *pvmove_mirror_count)
{
	struct mirror_state *mirr_state;
	uint32_t area_count = seg->area_count;
	unsigned start_area = 0u;
	int mirror_status = MIRR_RUNNING;
	uint32_t region_size;
	int r;

	if (!*target_state)
		*target_state = _mirrored_init_target(mem, cmd);

	mirr_state = *target_state;

	/*
	 * Mirror segment could have only 1 area temporarily
	 * if the segment is under conversion.
	 */
 	if (seg->area_count == 1)
		mirror_status = MIRR_DISABLED;

	/*
	 * For pvmove, only have one mirror segment RUNNING at once.
	 * Segments before this are COMPLETED and use 2nd area.
	 * Segments after this are DISABLED and use 1st area.
	 */
	if (seg->status & PVMOVE) {
		if (seg->extents_copied == seg->area_len) {
			mirror_status = MIRR_COMPLETED;
			start_area = 1;
		} else if ((*pvmove_mirror_count)++) {
			mirror_status = MIRR_DISABLED;
			area_count = 1;
		}
		/* else MIRR_RUNNING */
	}

	if (mirror_status != MIRR_RUNNING) {
		if (!dm_tree_node_add_linear_target(node, len))
			return_0;
		goto done;
	}

	if (!(seg->status & PVMOVE)) {
		if (!seg->region_size) {
			log_error("Missing region size for mirror segment.");
			return 0;
		}
		region_size = seg->region_size;

	} else
		region_size = adjusted_mirror_region_size(seg->lv->vg->extent_size,
							  seg->area_len,
							  mirr_state->default_region_size);

	if (!dm_tree_node_add_mirror_target(node, len))
		return_0;

	if ((r = _add_log(mem, seg, laopts, node, area_count, region_size)) <= 0) {
		stack;
		return r;
	}

      done:
	return add_areas_line(dm, seg, node, start_area, area_count);
}

static int _mirrored_target_present(struct cmd_context *cmd,
				    const struct lv_segment *seg,
				    unsigned *attributes)
{
	static int _mirrored_checked = 0;
	static int _mirrored_present = 0;
	uint32_t maj, min, patchlevel;
	unsigned maj2, min2, patchlevel2;
	char vsn[80];
	struct utsname uts;
	unsigned kmaj, kmin, krel;

	if (!_mirrored_checked) {
		_mirrored_present = target_present(cmd, "mirror", 1);

		/*
		 * block_on_error available as "block_on_error" log
		 * argument with mirror target >= 1.1 and <= 1.11
		 * or with 1.0 in RHEL4U3 driver >= 4.5
		 *
		 * block_on_error available as "handle_errors" mirror
		 * argument with mirror target >= 1.12.
		 *
		 * libdm-deptree.c is smart enough to handle the differences
		 * between block_on_error and handle_errors for all
		 * mirror target versions >= 1.1
		 */
		/* FIXME Move this into libdevmapper */

		if (target_version("mirror", &maj, &min, &patchlevel) &&
		    maj == 1 &&
		    ((min >= 1) ||
		     (min == 0 && driver_version(vsn, sizeof(vsn)) &&
		      sscanf(vsn, "%u.%u.%u", &maj2, &min2, &patchlevel2) == 3 &&
		      maj2 == 4 && min2 == 5 && patchlevel2 == 0)))	/* RHEL4U3 */
			_block_on_error_available = 1;
	}

	/*
	 * Check only for modules if atttributes requested and no previous check.
	 * FIXME: Fails incorrectly if cmirror was built into kernel.
	 */
	if (attributes) {
		if (!_mirror_attributes) {
			/*
			 * The dm-log-userspace module was added to the
			 * 2.6.31 kernel.
			 */
			if (!uname(&uts) &&
			    (sscanf(uts.release, "%u.%u.%u", &kmaj, &kmin, &krel) == 3) &&
			    KERNEL_VERSION(kmaj, kmin, krel) < KERNEL_VERSION(2, 6, 31)) {
				if (module_present(cmd, "log-clustered"))
					_mirror_attributes |= MIRROR_LOG_CLUSTERED;
			} else if (module_present(cmd, "log-userspace"))
				_mirror_attributes |= MIRROR_LOG_CLUSTERED;

			if (!(_mirror_attributes & MIRROR_LOG_CLUSTERED))
				log_verbose("Cluster mirror log module is not available");

			/*
			 * The cluster mirror log daemon must be running,
			 * otherwise, the kernel module will fail to make
			 * contact.
			 */
#ifdef CMIRRORD_PIDFILE
			if (!dm_daemon_is_running(CMIRRORD_PIDFILE)) {
				log_verbose("Cluster mirror log daemon is not running");
				_mirror_attributes &= ~MIRROR_LOG_CLUSTERED;
			}
#else
			log_verbose("Cluster mirror log daemon not included in build");
			_mirror_attributes &= ~MIRROR_LOG_CLUSTERED;
#endif
		}
		*attributes = _mirror_attributes;
	}
	_mirrored_checked = 1;

	return _mirrored_present;
}

#ifdef DMEVENTD
static const char *_get_mirror_dso_path(struct cmd_context *cmd)
{
	return get_monitor_dso_path(cmd, find_config_tree_str(cmd, "dmeventd/mirror_library",
							      DEFAULT_DMEVENTD_MIRROR_LIB));
}

/* FIXME Cache this */
static int _target_registered(struct lv_segment *seg, int *pending)
{
	return target_registered_with_dmeventd(seg->lv->vg->cmd, _get_mirror_dso_path(seg->lv->vg->cmd),
					       seg->lv, pending);
}

/* FIXME This gets run while suspended and performs banned operations. */
static int _target_set_events(struct lv_segment *seg, int evmask, int set)
{
	return target_register_events(seg->lv->vg->cmd, _get_mirror_dso_path(seg->lv->vg->cmd),
				      seg->lv, evmask, set, 0);
}

static int _target_monitor_events(struct lv_segment *seg, int events)
{
	return _target_set_events(seg, events, 1);
}

static int _target_unmonitor_events(struct lv_segment *seg, int events)
{
	return _target_set_events(seg, events, 0);
}

#endif /* DMEVENTD */
#endif /* DEVMAPPER_SUPPORT */

static int _mirrored_modules_needed(struct dm_pool *mem,
				    const struct lv_segment *seg,
				    struct dm_list *modules)
{
	if (seg->log_lv &&
	    !list_segment_modules(mem, first_seg(seg->log_lv), modules))
		return_0;

	if (vg_is_clustered(seg->lv->vg) &&
	    !str_list_add(mem, modules, "clog")) {
		log_error("cluster log string list allocation failed");
		return 0;
	}

	if (!str_list_add(mem, modules, "mirror")) {
		log_error("mirror string list allocation failed");
		return 0;
	}

	return 1;
}

static void _mirrored_destroy(struct segment_type *segtype)
{
	dm_free(segtype);
}

static struct segtype_handler _mirrored_ops = {
	.name = _mirrored_name,
	.display = _mirrored_display,
	.text_import_area_count = _mirrored_text_import_area_count,
	.text_import = _mirrored_text_import,
	.text_export = _mirrored_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _mirrored_add_target_line,
	.target_percent = _mirrored_target_percent,
	.target_present = _mirrored_target_present,
	.check_transient_status = _mirrored_transient_status,
#  ifdef DMEVENTD
	.target_monitored = _target_registered,
	.target_monitor_events = _target_monitor_events,
	.target_unmonitor_events = _target_unmonitor_events,
#  endif	/* DMEVENTD */
#endif
	.modules_needed = _mirrored_modules_needed,
	.destroy = _mirrored_destroy,
};

#ifdef MIRRORED_INTERNAL
struct segment_type *init_mirrored_segtype(struct cmd_context *cmd)
#else				/* Shared */
struct segment_type *init_segtype(struct cmd_context *cmd);
struct segment_type *init_segtype(struct cmd_context *cmd)
#endif
{
	struct segment_type *segtype = dm_zalloc(sizeof(*segtype));

	if (!segtype)
		return_NULL;

	segtype->cmd = cmd;
	segtype->ops = &_mirrored_ops;
	segtype->name = "mirror";
	segtype->private = NULL;
	segtype->flags = SEG_AREAS_MIRRORED;

#ifdef DEVMAPPER_SUPPORT
#  ifdef DMEVENTD
	if (_get_mirror_dso_path(cmd))
		segtype->flags |= SEG_MONITORED;
#  endif	/* DMEVENTD */
#endif

	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}
