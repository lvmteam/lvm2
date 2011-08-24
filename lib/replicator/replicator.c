/*
 * Copyright (C) 2009-2010 Red Hat, Inc. All rights reserved.
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

/* Dm kernel module name for replicator */
#define REPLICATOR_MODULE "replicator"
#define REPLICATOR_DEV_MODULE "replicator-dev"

/*
 * Macro used as return argument - returns 0.
 * return is left to be written in the function for better readability.
 */
#define SEG_LOG_ERROR(t, p...) \
	log_error(t " segment %s of logical volume %s.", ## p, \
		  config_parent_name(sn), seg->lv->name), 0;


/*
 *  Replicator target
 */
static const char *_replicator_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

/* FIXME: missing implementation */
static void _replicator_display(const struct lv_segment *seg)
{
	//const char *size;
	//uint32_t s;

	log_print("  Replicator");
	if (seg->rlog_lv)
		log_print("  Replicator volume\t%s", seg->rlog_lv->name);
}

/* Wrapper for get_config_uint32() with default value */
static uint32_t _get_config_uint32(const struct config_node *cn,
				   const char *path,
				   uint32_t def)
{
	uint32_t t;

	return get_config_uint32(cn, path, &t) ? t : def;
}

/* Wrapper for get_config_uint64() with default value */
static uint64_t _get_config_uint64(const struct config_node *cn,
				   const char *path,
				   uint64_t def)
{
	uint64_t t;

	return get_config_uint64(cn, path, &t) ? t : def;
}


/* Strings replicator_state_t enum */
static const char _state_txt[NUM_REPLICATOR_STATE][8] = {
	"passive",
	"active"
};

/* Parse state string */
static replicator_state_t _get_state(const struct config_node *sn,
				     const char *path, replicator_state_t def)
{
	const char *str;
	unsigned i;

	if (get_config_str(sn, path, &str)) {
		for (i = 0; i < sizeof(_state_txt)/sizeof(_state_txt[0]); ++i)
			if (strcasecmp(str, _state_txt[i]) == 0)
				return (replicator_state_t) i;

		log_warn("%s: unknown value '%s', using default '%s' state",
			 path, str, _state_txt[def]);
	}

	return def;
}

/* Strings for replicator_action_t enum */
static const char _op_mode_txt[NUM_DM_REPLICATOR_MODES][8] = {
	"sync",
	"warn",
	"stall",
	"drop",
	"fail"
};


/* Parse action string */
static dm_replicator_mode_t _get_op_mode(const struct config_node *sn,
					 const char *path, dm_replicator_mode_t def)
{
	const char *str;
	unsigned i;

	if (get_config_str(sn, path, &str)) {
		for (i = 0; i < sizeof(_op_mode_txt)/sizeof(_op_mode_txt[0]); ++i)
			if (strcasecmp(str, _op_mode_txt[i]) == 0) {
				log_very_verbose("Setting %s to %s",
						 path, _op_mode_txt[i]);
				return (dm_replicator_mode_t) i;
			}
		log_warn("%s: unknown value '%s', using default '%s' operation mode",
			 path, str, _op_mode_txt[def]);
	}

	return def;
}

static struct replicator_site *_get_site(struct logical_volume *replicator,
					 const char *key)
{
	struct dm_pool *mem = replicator->vg->vgmem;
	struct replicator_site *rsite;

	dm_list_iterate_items(rsite, &replicator->rsites)
		if (strcasecmp(rsite->name, key) == 0)
			return rsite;

	if (!(rsite = dm_pool_zalloc(mem, sizeof(*rsite))))
		return_NULL;

	if (!(rsite->name = dm_pool_strdup(mem, key)))
		return_NULL;

	rsite->replicator = replicator;
	dm_list_init(&rsite->rdevices);
	dm_list_add(&replicator->rsites, &rsite->list);

	return rsite;
}


/* Parse replicator site element */
static int _add_site(struct lv_segment *seg,
		     const char *key,
		     const struct config_node *sn)
{
	struct dm_pool *mem = seg->lv->vg->vgmem;
	const struct config_node *cn;
	struct replicator_site *rsite;

	if (!(rsite = _get_site(seg->lv, key)))
		return_0;

	if (!find_config_node(sn, "site_index"))
		return SEG_LOG_ERROR("Mandatory site_index is missing for");

	rsite->state = _get_state(sn, "state", REPLICATOR_STATE_PASSIVE);
	rsite->site_index = _get_config_uint32(sn, "site_index", 0);
	if (rsite->site_index > seg->rsite_index_highest)
		return SEG_LOG_ERROR("site_index=%d > highest_site_index=%d for",
				     rsite->site_index, seg->rsite_index_highest);

	rsite->fall_behind_data = _get_config_uint64(sn, "fall_behind_data", 0);
	rsite->fall_behind_ios = _get_config_uint32(sn, "fall_behind_ios", 0);
	rsite->fall_behind_timeout = _get_config_uint32(sn, "fall_behind_timeout", 0);
	rsite->op_mode = DM_REPLICATOR_SYNC;

	if (rsite->fall_behind_data ||
	    rsite->fall_behind_ios ||
	    rsite->fall_behind_timeout) {
		if (rsite->fall_behind_data && rsite->fall_behind_ios)
			return SEG_LOG_ERROR("Defined both fall_behind_data "
					     "and fall_behind_ios in");

		if (rsite->fall_behind_data && rsite->fall_behind_timeout)
			return SEG_LOG_ERROR("Defined both fall_behind_data "
					     "and fall_behind_timeout in");

		if (rsite->fall_behind_ios && rsite->fall_behind_timeout)
			return SEG_LOG_ERROR("Defined both fall_behind_ios "
					     "and fall_behind_timeout in");

		rsite->op_mode = _get_op_mode(sn, "operation_mode",
					      rsite->op_mode);
	}

	if ((cn = find_config_node(sn, "volume_group"))) {
		if (!cn->v || cn->v->type != CFG_STRING)
			return SEG_LOG_ERROR("volume_group must be a string in");

		if (!(rsite->vg_name = dm_pool_strdup(mem, cn->v->v.str)))
			return_0;

	} else if (rsite->site_index != 0)
		return SEG_LOG_ERROR("volume_group is mandatory for remote site in");

	return 1;
}


/* Import replicator segment */
static int _replicator_text_import(struct lv_segment *seg,
				   const struct config_node *sn,
				   struct dm_hash_table *pv_hash __attribute__((unused)))
{
	const struct config_node *cn;
	struct logical_volume *rlog_lv;

	if (!replicator_add_replicator_dev(seg->lv, NULL))
		return_0;

	if (!(cn = find_config_node(sn, "replicator_log")) ||
	    !cn->v || cn->v->type != CFG_STRING)
		return SEG_LOG_ERROR("Replicator log type must be a string in");

	if (!(rlog_lv = find_lv(seg->lv->vg, cn->v->v.str)))
		return SEG_LOG_ERROR("Unknown replicator log %s in",
				     cn->v->v.str);

	if (!(cn = find_config_node(sn, "replicator_log_type")) ||
	    !cn->v || cn->v->type != CFG_STRING)
		return SEG_LOG_ERROR("Replicator log's type must be a string in");
	if (strcasecmp(cn->v->v.str, "ringbuffer"))
		return SEG_LOG_ERROR("Only ringbuffer replicator log type is supported in");

	if (!(seg->rlog_type = dm_pool_strdup(seg->lv->vg->vgmem, cn->v->v.str)))
		return_0;


	log_very_verbose("replicator_log = %s", rlog_lv->name);
	log_very_verbose("replicator_log_type = %s", seg->rlog_type);

	if (!replicator_add_rlog(seg, rlog_lv))
		return_0;

	seg->rdevice_index_highest = _get_config_uint64(sn, "highest_device_index", 0);
	seg->rsite_index_highest = _get_config_uint32(sn, "highest_site_index", 0);

	seg->region_size = _get_config_uint32(sn, "sync_log_size", 0);

	for (; sn; sn = sn->sib)
		if (!sn->v) {
			for (cn = sn->sib; cn; cn = cn->sib)
				if (!cn->v && (strcasecmp(cn->key ,sn->key) == 0))
					return SEG_LOG_ERROR("Detected duplicate site "
							     "name %s in", sn->key);
			if (!_add_site(seg, sn->key, sn->child))
				return_0;
		}
	return 1;
}

/* Export replicator segment */
static int _replicator_text_export(const struct lv_segment *seg,
				   struct formatter *f)
{
	struct replicator_site *rsite;

	if (!seg->rlog_lv)
                return_0;

	outf(f, "replicator_log = \"%s\"", seg->rlog_lv->name);
	outf(f, "replicator_log_type = \"%s\"", seg->rlog_type);
	outf(f, "highest_device_index = %" PRIu64, seg->rdevice_index_highest);
	outf(f, "highest_site_index = %d", seg->rsite_index_highest);

	if (seg->region_size)
		outsize(f, (uint64_t)seg->region_size,
			"sync_log_size = %" PRIu32, seg->region_size);

	if (!dm_list_empty(&seg->lv->rsites))
		outnl(f);

	dm_list_iterate_items(rsite, &seg->lv->rsites) {
		outf(f, "%s {", rsite->name);
		out_inc_indent(f);

		outf(f, "state = \"%s\"", _state_txt[rsite->state]);
		outf(f, "site_index = %d", rsite->site_index);

		/* Only non-default parameters are written */
		if (rsite->op_mode != DM_REPLICATOR_SYNC)
			outf(f, "operation_mode = \"%s\"",
			     _op_mode_txt[rsite->op_mode]);
		if (rsite->fall_behind_timeout)
			outfc(f, "# seconds", "fall_behind_timeout = %u",
			     rsite->fall_behind_timeout);
		if (rsite->fall_behind_ios)
			outfc(f, "# io operations", "fall_behind_ios = %u",
			     rsite->fall_behind_ios);
		if (rsite->fall_behind_data)
			outsize(f, rsite->fall_behind_data, "fall_behind_data = %" PRIu64,
				rsite->fall_behind_data);
		if (rsite->state != REPLICATOR_STATE_ACTIVE && rsite->vg_name)
			outf(f, "volume_group = \"%s\"", rsite->vg_name);

		out_dec_indent(f);
		outf(f, "}");
	}

	return 1;
}

#ifdef DEVMAPPER_SUPPORT
static int _replicator_add_target_line(struct dev_manager *dm,
				       struct dm_pool *mem,
				       struct cmd_context *cmd,
				       void **target_state,
				       struct lv_segment *seg,
				       const struct lv_activate_opts *laopts,
				       struct dm_tree_node *node,
				       uint64_t len,
				       uint32_t *pvmove_mirror_count)
{
	const char *rlog_dlid;
	struct replicator_site *rsite;

	if (!seg->rlog_lv)
		return_0;

	if (!(rlog_dlid = build_dm_uuid(mem, seg->rlog_lv->lvid.s, NULL)))
		return_0;

	dm_list_iterate_items(rsite, &seg->lv->rsites) {
		if (!dm_tree_node_add_replicator_target(node,
							seg->rlog_lv->size,
							rlog_dlid,
							seg->rlog_type,
							rsite->site_index,
							rsite->op_mode,
							rsite->fall_behind_timeout,
							rsite->fall_behind_data,
							rsite->fall_behind_ios)) {
			if (rsite->site_index == 0) {
				log_error("Failed to add replicator log '%s' "
					  "to replicator '%s'.",
					  rlog_dlid, seg->lv->name);
				return 0;
			}
			// FIXME:
		}
	}

	return 1;
}

/* FIXME: write something useful for replicator here */
static int _replicator_target_percent(void **target_state,
				      percent_t *percent,
				      struct dm_pool *mem,
				      struct cmd_context *cmd,
				      struct lv_segment *seg,
				      char *params, uint64_t *total_numerator,
				      uint64_t *total_denominator)
{
	return 1;
}

/* Check for module presence */
static int _replicator_target_present(struct cmd_context *cmd,
				      const struct lv_segment *seg __attribute__((unused)),
				      unsigned *attributes __attribute__((unused)))
{
	static int _checked = 0;
	static int _present = 0;

	if (!_checked) {
		_present = target_present(cmd, REPLICATOR_MODULE, 1);
		_checked = 1;
	}

	return _present;
}

#endif

static int _replicator_modules_needed(struct dm_pool *mem,
				      const struct lv_segment *seg __attribute__((unused)),
				      struct dm_list *modules)
{
	if (!str_list_add(mem, modules, REPLICATOR_MODULE))
		return_0;

	if (!str_list_add(mem, modules, REPLICATOR_DEV_MODULE))
		return_0;

	return 1;
}

static void _replicator_destroy(struct segment_type *segtype)
{
	dm_free(segtype);
}

static struct segtype_handler _replicator_ops = {
	.name = _replicator_name,
	.display = _replicator_display,
	.text_import = _replicator_text_import,
	.text_export = _replicator_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _replicator_add_target_line,
	.target_percent = _replicator_target_percent,
	.target_present = _replicator_target_present,
#endif
	.modules_needed = _replicator_modules_needed,
	.destroy = _replicator_destroy,
};

/*
 *  Replicator-dev  target
 */
static void _replicator_dev_display(const struct lv_segment *seg)
{
	//const char *size;
	//uint32_t s;
	// FIXME: debug test code for now
	log_print("  Replicator\t\t%u", seg->area_count);
	log_print("  Mirror size\t\t%u", seg->area_len);
	if (seg->log_lv)
		log_print("  Replicator log volume\t%s", seg->rlog_lv->name);

}

static int _add_device(struct lv_segment *seg,
		       const char *site_name,
		       const struct config_node *sn,
		       uint64_t devidx)
{
	struct dm_pool *mem = seg->lv->vg->vgmem;
	struct logical_volume *lv = NULL;
	struct logical_volume *slog_lv = NULL;
	struct replicator_site *rsite = _get_site(seg->replicator, site_name);
	struct replicator_device *rdev;
	const char *dev_str = NULL;
	const char *slog_str = NULL;
	const struct config_node *cn;

	dm_list_iterate_items(rdev, &rsite->rdevices)
		if (rdev->replicator_dev == seg)
			return SEG_LOG_ERROR("Duplicate site found in");

	if ((cn = find_config_node(sn, "sync_log"))) {
		if (!cn->v || !cn->v->v.str)
			return SEG_LOG_ERROR("Sync log must be a string in");
		slog_str = cn->v->v.str;
	}

	if (!(cn = find_config_node(sn, "logical_volume")) ||
	    !cn->v || !cn->v->v.str)
		return SEG_LOG_ERROR("Logical volume must be a string in");

	dev_str = cn->v->v.str;

	if (!seg->lv->rdevice) {
		if (slog_str)
			return SEG_LOG_ERROR("Sync log %s defined for local "
					     "device in", slog_str);

		/* Check for device in current VG */
		if (!(lv = find_lv(seg->lv->vg, dev_str)))
			return SEG_LOG_ERROR("Logical volume %s not found in",
					     dev_str);
	} else {
		if (!slog_str)
			return SEG_LOG_ERROR("Sync log is missing for remote "
					     "device in");
		/* Check for slog device in current VG */
		if (!(slog_lv = find_lv(seg->lv->vg, slog_str)))
			return SEG_LOG_ERROR("Sync log %s not found in",
					     slog_str);
	}

	if (!(rdev = dm_pool_zalloc(mem, sizeof(*rdev))))
		return_0;

	if (!(rdev->name = dm_pool_strdup(mem, dev_str)))
		return_0;

	rdev->replicator_dev = seg;
	rdev->rsite = rsite;
	rdev->device_index = devidx;

	if (!seg->lv->rdevice) {
		if (!replicator_dev_add_rimage(rdev, lv))
			return SEG_LOG_ERROR("LV inconsistency found in");
		seg->lv->rdevice = rdev;
	} else {
		if (!slog_str ||
		    !(rdev->slog_name = dm_pool_strdup(mem, slog_str)))
			return_0;

		if (!replicator_dev_add_slog(rdev, slog_lv))
			return SEG_LOG_ERROR("Sync log inconsistency found in");
	}

	dm_list_add(&rsite->rdevices, &rdev->list);// linked site list

	return 1;
}

/* Import replicator segment */
static int _replicator_dev_text_import(struct lv_segment *seg,
				       const struct config_node *sn,
				       struct dm_hash_table *pv_hash __attribute__((unused)))
{
	const struct config_node *cn;
	struct logical_volume *replicator;
	uint64_t devidx;

	if (!(cn = find_config_node(sn, "replicator")))
		return SEG_LOG_ERROR("Replicator is missing for");

	if (!cn->v || !cn->v->v.str)
		return SEG_LOG_ERROR("Replicator must be a string for");

	if (!(replicator = find_lv(seg->lv->vg, cn->v->v.str)))
		return SEG_LOG_ERROR("Unknown replicator %s for", cn->v->v.str);

	if (!replicator_add_replicator_dev(replicator, seg))
		return_0;

	log_very_verbose("replicator=%s", replicator->name);

	/* Mandatory */
	if (!find_config_node(sn, "device_index") ||
	    !get_config_uint64(sn, "device_index", &devidx))
		return SEG_LOG_ERROR("Could not read 'device_index' for");

	/* Read devices from sites */
	for (; sn; sn = sn->sib)
		if (!(sn->v) && !_add_device(seg, sn->key, sn->child, devidx))
			return_0;

	if (!seg->lv->rdevice)
		return SEG_LOG_ERROR("Replicator device without site in");

	seg->rlog_lv = NULL;
	seg->lv->status |= REPLICATOR;

	return 1;
}

/* Export replicator-dev segment */
static int _replicator_dev_text_export(const struct lv_segment *seg,
				       struct formatter *f)
{
	struct replicator_site *rsite;
	struct replicator_device *rdev;

	if (!seg->replicator || !seg->lv->rdevice)
		return_0;

	outf(f, "replicator = \"%s\"", seg->replicator->name);
	outf(f, "device_index = %" PRId64, seg->lv->rdevice->device_index);

	outnl(f);

	dm_list_iterate_items(rsite, &seg->replicator->rsites) {
		dm_list_iterate_items(rdev, &rsite->rdevices) {
			if (rdev->replicator_dev != seg)
				continue;

			outf(f, "%s {", rdev->rsite->name);

			out_inc_indent(f);

			outf(f, "logical_volume = \"%s\"",
			     rdev->name ? rdev->name : rdev->lv->name);

			if (rdev->slog)
				outf(f, "sync_log = \"%s\"", rdev->slog->name);
			else if (rdev->slog_name)
				outf(f, "sync_log = \"%s\"", rdev->slog_name);

			out_dec_indent(f);

			outf(f, "}");
		}
	}

	return 1;
}

#ifdef DEVMAPPER_SUPPORT
/*
 * Add target for passive site matching the device index
 */
static int _replicator_dev_add_target_line(struct dev_manager *dm,
					   struct dm_pool *mem,
					   struct cmd_context *cmd,
					   void **target_state,
					   struct lv_segment *seg,
					   const struct lv_activate_opts *laopts,
					   struct dm_tree_node *node,
					   uint64_t len,
					   uint32_t *pvmove_mirror_count)
{
	const char *replicator_dlid, *rdev_dlid, *slog_dlid;
	struct replicator_device *rdev, *rdev_search;
	struct replicator_site *rsite;
	uint32_t slog_size;
	uint32_t slog_flags;

	if (!lv_is_active_replicator_dev(seg->lv)) {
		/* Create passive linear mapping */
		log_very_verbose("Inactive replicator %s using %s.",
				 seg->lv->name, seg->lv->rdevice->lv->name);
		if (!dm_tree_node_add_linear_target(node, seg->lv->size))
			return_0;
		if (!(rdev_dlid = build_dm_uuid(mem, seg->lv->rdevice->lv->lvid.s, NULL)))
			return_0;
		return dm_tree_node_add_target_area(node, NULL, rdev_dlid, 0);
	} else if (seg->lv->rdevice->rsite->site_index) {
		log_error("Active site with site_index != 0 (%s, %d)",
			  seg->lv->rdevice->rsite->name,
			  seg->lv->rdevice->rsite->site_index);
		return 0; /* Replicator without any active site */
	}

	/*
	 * At this point all devices that have some connection with replicator
	 * must be present in dm_tree
	 */
	if (!seg_is_replicator_dev(seg) ||
	    !(replicator_dlid = build_dm_uuid(mem, seg->replicator->lvid.s, NULL)))
		return_0;

	/* Select remote devices with the same device index */
	dm_list_iterate_items(rsite, &seg->replicator->rsites) {
		if (rsite->site_index == 0) {
			/* Local slink0 device */
			rdev = seg->lv->rdevice;
		} else {
			rdev = NULL;
			dm_list_iterate_items(rdev_search, &rsite->rdevices) {
				if (rdev_search->replicator_dev == seg) {
					rdev = rdev_search;
					break;
				}
			}

			if (!rdev) {
				log_error(INTERNAL_ERROR "rdev list not found.");
				return 0;
			}
		}

		if (!rdev->lv ||
		    !(rdev_dlid = build_dm_uuid(mem, rdev->lv->lvid.s, NULL)))
			return_0;

		slog_dlid = NULL;

		/* Using either disk or core (in memory) log */
		if (rdev->slog) {
			slog_flags = DM_NOSYNC;
			slog_size = (uint32_t) rdev->slog->size;
			if (!(slog_dlid = build_dm_uuid(mem, rdev->slog->lvid.s, NULL)))
				return_0;
		} else if (rdev->slog_name &&
			   sscanf(rdev->slog_name, "%" PRIu32, &slog_size) == 1) {
			slog_flags = DM_CORELOG | DM_FORCESYNC;
			if (slog_size == 0) {
				log_error("Failed to use empty corelog size "
					  "in replicator '%s'.",
					  rsite->replicator->name);
				return 0;
			}
		} else  {
			slog_flags = DM_CORELOG | DM_FORCESYNC;
			slog_size = 0; /* NOLOG */
		}

		if (!dm_tree_node_add_replicator_dev_target(node,
							    seg->lv->size,
							    replicator_dlid,
							    seg->lv->rdevice->device_index,
							    rdev_dlid,
							    rsite->site_index,
							    slog_dlid,
							    slog_flags,
							    slog_size)) {
			return_0;
			/* FIXME: handle 'state = dropped' in future */
		}
	}

	return 1;
}

/* FIXME: write something useful for replicator-dev here */
static int _replicator_dev_target_percent(void **target_state,
					  percent_t *percent,
					  struct dm_pool *mem,
					  struct cmd_context *cmd,
					  struct lv_segment *seg,
					  char *params,
					  uint64_t *total_numerator,
					  uint64_t *total_denominator)
{
	return 1;
}

/* Check for module presence */
static int _replicator_dev_target_present(struct cmd_context *cmd,
					  const struct lv_segment *seg __attribute__((unused)),
					  unsigned *attributes __attribute__((unused)))
{
	static int _checked = 0;
	static int _present = 0;

	if (!_checked) {
		_present = target_present(cmd, REPLICATOR_DEV_MODULE, 1);
		_checked = 1;
	}

	return _present;
}

#endif

static struct segtype_handler _replicator_dev_ops = {
	.name = _replicator_name,
	.display = _replicator_dev_display,
	.text_import = _replicator_dev_text_import,
	.text_export = _replicator_dev_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _replicator_dev_add_target_line,
	.target_percent = _replicator_dev_target_percent,
	.target_present = _replicator_dev_target_present,
#endif
	.modules_needed = _replicator_modules_needed,
	.destroy = _replicator_destroy,
};

#ifdef REPLICATOR_INTERNAL
int init_replicator_segtype(struct cmd_context *cmd, struct segtype_library *seglib)
#else /* Shared */
int init_multiple_segtype(struct cmd_context *cmd, struct segtype_library *seglib);
int init_multiple_segtype(struct cmd_context *cmd, struct segtype_library *seglib)
#endif
{
	struct segment_type *segtype;

	if (!(segtype = dm_zalloc(sizeof(*segtype))))
		return_0;

	segtype->ops = &_replicator_ops;
	segtype->name = REPLICATOR_MODULE;
	segtype->private = NULL;
	segtype->flags = SEG_REPLICATOR;

	if (!lvm_register_segtype(seglib, segtype))
		return_0;

	log_very_verbose("Initialised segtype: " REPLICATOR_MODULE);

	if (!(segtype = dm_zalloc(sizeof(*segtype))))
		return_0;

	segtype->ops = &_replicator_dev_ops;
	segtype->name = REPLICATOR_DEV_MODULE;
	segtype->private = NULL;
	segtype->flags = SEG_REPLICATOR_DEV;

	if (!lvm_register_segtype(seglib, segtype))
		return_0;

	log_very_verbose("Initialised segtype: " REPLICATOR_DEV_MODULE);

	return 1;
}
