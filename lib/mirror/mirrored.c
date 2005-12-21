/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.  
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

#ifdef DMEVENTD
#  include <libdevmapper-event.h>
#endif

static int _block_on_error_available = 0;

enum {
	MIRR_DISABLED,
	MIRR_RUNNING,
	MIRR_COMPLETED
};

struct mirror_state {
	uint32_t default_region_size;
};

static const char *_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static void _display(const struct lv_segment *seg)
{
	const char *size;
	uint32_t s;

	log_print("  Mirrors\t\t%u", seg->area_count);
	log_print("  Mirror size\t\t%u", seg->area_len);
	if (seg->log_lv)
		log_print("  Mirror log volume\t%s", seg->log_lv->name);

	if (seg->region_size) {
		size = display_size(seg->lv->vg->cmd,
				    (uint64_t) seg->region_size,
				    SIZE_SHORT);
		log_print("  Mirror region size\t%s", size);
	}

	log_print("  Mirror original:");
	display_stripe(seg, 0, "    ");
	log_print("  Mirror destinations:");
	for (s = 1; s < seg->area_count; s++)
		display_stripe(seg, s, "    ");
	log_print(" ");
}

static int _text_import_area_count(struct config_node *sn, uint32_t *area_count)
{
	if (!get_config_uint32(sn, "mirror_count", area_count)) {
		log_error("Couldn't read 'mirror_count' for "
			  "segment '%s'.", sn->key);
		return 0;
	}

	return 1;
}

static int _text_import(struct lv_segment *seg, const struct config_node *sn,
			struct dm_hash_table *pv_hash)
{
	const struct config_node *cn;
	char *logname = NULL;

	if (find_config_node(sn, "extents_moved")) {
		if (get_config_uint32(sn, "extents_moved",
				      &seg->extents_copied))
			seg->status |= PVMOVE;
		else {
			log_error("Couldn't read 'extents_moved' for "
				  "segment '%s'.", sn->key);
			return 0;
		}
	}

	if (find_config_node(sn, "region_size")) {
		if (!get_config_uint32(sn, "region_size",
				      &seg->region_size)) {
			log_error("Couldn't read 'region_size' for "
				  "segment '%s'.", sn->key);
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
			log_error("Unrecognised mirror log in segment %s.",
				  sn->key);
			return 0;
		}
		seg->log_lv->status |= MIRROR_LOG;
        }

	if (logname && !seg->region_size) {
		log_error("Missing region size for mirror log for segment "
			  "'%s'.", sn->key);
		return 0;
	}

	if (!(cn = find_config_node(sn, "mirrors"))) {
		log_error("Couldn't find mirrors array for segment "
			  "'%s'.", sn->key);
		return 0;
	}

	return text_import_areas(seg, sn, cn, pv_hash, MIRROR_IMAGE);
}

static int _text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "mirror_count = %u", seg->area_count);
	if (seg->status & PVMOVE)
		out_size(f, (uint64_t) seg->extents_copied * seg->lv->vg->extent_size,
			 "extents_moved = %" PRIu32, seg->extents_copied);
	if (seg->log_lv)
		outf(f, "mirror_log = \"%s\"", seg->log_lv->name);
	if (seg->region_size)
		outf(f, "region_size = %" PRIu32, seg->region_size);

	return out_areas(f, seg, "mirror");
}

#ifdef DEVMAPPER_SUPPORT
static struct mirror_state *_init_target(struct dm_pool *mem,
					 struct config_tree *cft)
{
	struct mirror_state *mirr_state;

	if (!(mirr_state = dm_pool_alloc(mem, sizeof(*mirr_state)))) {
		log_error("struct mirr_state allocation failed");
		return NULL;
	}

	mirr_state->default_region_size = 2 *
	    find_config_int(cft->root,
			    "activation/mirror_region_size",
			    DEFAULT_MIRROR_REGION_SIZE);

	return mirr_state;
}

static int _target_percent(void **target_state, struct dm_pool *mem,
			   struct config_tree *cft, struct lv_segment *seg,
			   char *params, uint64_t *total_numerator,
			   uint64_t *total_denominator, float *percent)
{
	struct mirror_state *mirr_state;
	uint64_t numerator, denominator;
	unsigned mirror_count, m;
	int used;
	char *pos = params;

	if (!*target_state)
		*target_state = _init_target(mem, cft);

	mirr_state = *target_state;

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

	return 1;
}

static int _add_log(struct dev_manager *dm, struct lv_segment *seg,
		    struct dm_tree_node *node, uint32_t area_count, uint32_t region_size)
{
	unsigned clustered = 0;
	char *log_dlid = NULL;
	uint32_t log_flags = 0;

	/*
	 * Use clustered mirror log for non-exclusive activation 
	 * in clustered VG.
	 */
	if ((!(seg->lv->status & ACTIVATE_EXCL) &&
	      (seg->lv->vg->status & CLUSTERED)))
		clustered = 1;

	if (seg->log_lv &&
	    !(log_dlid = build_dlid(dm, seg->log_lv->lvid.s, NULL))) {
		log_error("Failed to build uuid for log LV %s.",
			  seg->log_lv->name);
		return 0;
	}

	if (_block_on_error_available && !(seg->status & PVMOVE))
		log_flags |= DM_BLOCK_ON_ERROR;

	return dm_tree_node_add_mirror_target_log(node, region_size, clustered, log_dlid, area_count, log_flags);
}

static int _add_target_line(struct dev_manager *dm, struct dm_pool *mem,
                                struct config_tree *cft, void **target_state,
                                struct lv_segment *seg,
                                struct dm_tree_node *node, uint64_t len,
                                uint32_t *pvmove_mirror_count)
{
	struct mirror_state *mirr_state;
	uint32_t area_count = seg->area_count;
	int start_area = 0u;
	int mirror_status = MIRR_RUNNING;
	uint32_t region_size, region_max;
	int r;

	if (!*target_state)
		*target_state = _init_target(mem, cft);

	mirr_state = *target_state;

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
	} else {
		/* Find largest power of 2 region size unit we can use */
		region_max = (1 << (ffs(seg->area_len) - 1)) *
		      seg->lv->vg->extent_size;

		region_size = mirr_state->default_region_size;
		if (region_max < region_size) {
			region_size = region_max;
			log_verbose("Using reduced mirror region size of %u sectors",
				    region_size);
		}
	}

	if (!dm_tree_node_add_mirror_target(node, len))
		return_0;

	if ((r = _add_log(dm, seg, node, area_count, region_size)) <= 0) {
		stack;
		return r;
	}

      done:
	return add_areas_line(dm, seg, node, start_area, area_count);
}

static int _target_present(void)
{
	static int checked = 0;
	static int present = 0;
	uint32_t maj, min, patchlevel;
	unsigned maj2, min2;
        char vsn[80];

	if (!checked) {
		present = target_present("mirror", 1);

		/*
		 * block_on_error available with mirror target >= 1.1
		 * or with 1.0 in RHEL4U3 driver >= 4.5
		 */
		/* FIXME Move this into libdevmapper */

		if (target_version("mirror", &maj, &min, &patchlevel) &&
		    maj == 1 && 
		    (min >= 1 || 
		     (min == 0 && driver_version(vsn, sizeof(vsn)) &&
		      sscanf(vsn, "%u.%u", &maj2, &min2) == 2 &&
		      maj2 >= 4 && min2 >= 5)))	/* RHEL4U3 */
			_block_on_error_available = 1;
	}

	checked = 1;

	return present;
}

#ifdef DMEVENTD
static int _setup_registration(struct dm_pool *mem, struct config_tree *cft,
			       char **dso)
{
	/* FIXME Follow lvm2 searching rules (see sharedlib.c) */
	/* FIXME Use naming convention in config file */
	if (!(*dso = find_config_str(cft->root, "global/mirror_dso", NULL))) {
		log_error("No mirror dso specified in config file");	/* FIXME readability */
		return 0;
	}

	return 1;
}

/* FIXME This gets run while suspended and performs banned operations. */
/* FIXME Merge these two functions */
static int _target_register_events(struct dm_pool *mem,
				   struct lv_segment *seg,
				   struct config_tree *cft, int events)
{
	char *dso;
	char dm_name[PATH_MAX];
	struct logical_volume *lv;
	struct volume_group *vg;
	int err;

	lv = seg->lv;
	vg = lv->vg;

	if (!_setup_registration(mem, cft, &dso)) {
		stack;
		return 0;
	}

	/* FIXME lvm_ error */
	strncpy(dm_name, build_dm_name(mem, vg->name, lv->name, NULL),
		PATH_MAX);

	if((err = dm_event_register(dso, dm_name, DM_EVENT_ALL_ERRORS)) < 0) {
		log_error("Unable to register %s for events: %s", dm_name,
			  strerror(-err));
		return 0;
	}
	log_info("Registered %s for events", dm_name);

	return 1;
}

static int _target_unregister_events(struct dm_pool *mem,
				     struct lv_segment *seg,
				     struct config_tree *cft, int events)
{
	char *dso;
	char devpath[PATH_MAX];
	struct logical_volume *lv;
	struct volume_group *vg;
	int err;

	lv = seg->lv;
	vg = lv->vg;

	if(!_setup_registration(mem, cft, &dso)) {
		stack;
		return 0;
	}

	/* FIXME lvm_ error */
	strncpy(devpath, build_dm_name(mem, vg->name, lv->name, NULL),
		PATH_MAX);

	/* FIXME put MIR_DSO into config file */
	if ((err = dm_event_unregister(dso, devpath, DM_EVENT_ALL_ERRORS)) < 0) {
		log_error("Unable to unregister %s for events: %s", devpath, strerror(-err));
		return 0;
	}

	log_info("Unregistered %s for events", devpath);

	return 1;
}

#endif /* DMEVENTD */
#endif /* DEVMAPPER_SUPPORT */

static void _destroy(const struct segment_type *segtype)
{
	dm_free((void *) segtype);
}

static struct segtype_handler _mirrored_ops = {
	name:_name,
	display:_display,
	text_import_area_count:_text_import_area_count,
	text_import:_text_import,
	text_export:_text_export,
#ifdef DEVMAPPER_SUPPORT
	add_target_line:_add_target_line,
	target_percent:_target_percent,
	target_present:_target_present,
#ifdef DMEVENTD
	target_register_events:_target_register_events,
	target_unregister_events:_target_unregister_events,
#endif
#endif
	destroy:_destroy,
};

#ifdef MIRRORED_INTERNAL
struct segment_type *init_mirrored_segtype(struct cmd_context *cmd)
#else				/* Shared */
struct segment_type *init_segtype(struct cmd_context *cmd);
struct segment_type *init_segtype(struct cmd_context *cmd)
#endif
{
	struct segment_type *segtype = dm_malloc(sizeof(*segtype));

	if (!segtype) {
		stack;
		return NULL;
	}

	segtype->cmd = cmd;
	segtype->ops = &_mirrored_ops;
	segtype->name = "mirror";
	segtype->private = NULL;
	segtype->flags = SEG_AREAS_MIRRORED;

	log_very_verbose("Initialised segtype: %s", segtype->name);

	return segtype;
}
