/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
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
#include "dev_manager.h"
#include "lvm-string.h"
#include "fs.h"
#include "defaults.h"
#include "segtype.h"
#include "display.h"
#include "toolcontext.h"
#include "targets.h"
#include "config.h"
#include "activate.h"
#include "lvm-exec.h"

#include <limits.h>
#include <dirent.h>

#define MAX_TARGET_PARAMSIZE 50000

typedef enum {
	PRELOAD,
	ACTIVATE,
	DEACTIVATE,
	SUSPEND,
	SUSPEND_WITH_LOCKFS,
	CLEAN
} action_t;

struct dev_manager {
	struct dm_pool *mem;

	struct cmd_context *cmd;

	void *target_state;
	uint32_t pvmove_mirror_count;
	int flush_required;
	int activation;                 /* building activation tree */
	int skip_external_lv;
	unsigned track_pvmove_deps;

	char *vg_name;
};

struct lv_layer {
	struct logical_volume *lv;
	const char *old_name;
};

int read_only_lv(struct logical_volume *lv, struct lv_activate_opts *laopts)
{
	return (laopts->read_only || !(lv->vg->status & LVM_WRITE) || !(lv->status & LVM_WRITE));
}

/*
 * Low level device-layer operations.
 */
static struct dm_task *_setup_task(const char *name, const char *uuid,
				   uint32_t *event_nr, int task,
				   uint32_t major, uint32_t minor)
{
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task)))
		return_NULL;

	if (name && !dm_task_set_name(dmt, name))
		goto_out;

	if (uuid && *uuid && !dm_task_set_uuid(dmt, uuid))
		goto_out;

	if (event_nr && !dm_task_set_event_nr(dmt, *event_nr))
		goto_out;

	if (major && !dm_task_set_major_minor(dmt, major, minor, 1))
		goto_out;

	if (activation_checks() && !dm_task_enable_checks(dmt))
		goto_out;
		
	return dmt;
      out:
	dm_task_destroy(dmt);
	return NULL;
}

static int _info_run(const char *name, const char *dlid, struct dm_info *info,
		     uint32_t *read_ahead, int mknodes, int with_open_count,
		     int with_read_ahead, uint32_t major, uint32_t minor)
{
	int r = 0;
	struct dm_task *dmt;
	int dmtask;

	dmtask = mknodes ? DM_DEVICE_MKNODES : DM_DEVICE_INFO;

	if (!(dmt = _setup_task(mknodes ? name : NULL, dlid, 0, dmtask, major, minor)))
		return_0;

	if (!with_open_count)
		if (!dm_task_no_open_count(dmt))
			log_error("Failed to disable open_count");

	if (!dm_task_run(dmt))
		goto_out;

	if (!dm_task_get_info(dmt, info))
		goto_out;

	if (with_read_ahead && info->exists) {
		if (!dm_task_get_read_ahead(dmt, read_ahead))
			goto_out;
	} else if (read_ahead)
		*read_ahead = DM_READ_AHEAD_NONE;

	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;
}

/*
 * _parse_mirror_status
 * @mirror_status_string
 * @image_health:  return for allocated copy of image health characters
 * @log_device: return for 'dev_t' of log device
 * @log_health: NULL if corelog, otherwise dm_malloc'ed log health char which
 *              the caller must free
 *
 * This function takes the mirror status string, breaks it up and returns
 * its components.  For now, we only return the health characters.  This
 * is an internal function.  If there are more things we want to return
 * later, we can do that then.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _parse_mirror_status(char *mirror_status_str,
				char **images_health,
				dev_t *log_dev, char **log_health)
{
	int major, minor;
	char *p = NULL;
	char **args, **log_args;
	unsigned num_devs, log_argc;

	*images_health = NULL;
	*log_health = NULL;
	*log_dev = 0;

	if (!dm_split_words(mirror_status_str, 1, 0, &p) ||
	    !(num_devs = (unsigned) atoi(p)))
		/* On errors, we must assume the mirror is to be avoided */
		return_0;

	p += strlen(p) + 1;
	args = alloca((num_devs + 5) * sizeof(char *));

	if ((unsigned)dm_split_words(p, num_devs + 4, 0, args) < num_devs + 4)
		return_0;

	log_argc = (unsigned) atoi(args[3 + num_devs]);
	log_args = alloca(log_argc * sizeof(char *));

	if ((unsigned)dm_split_words(args[3 + num_devs] + strlen(args[3 + num_devs]) + 1,
				     log_argc, 0, log_args) < log_argc)
		return_0;

	if (!strcmp(log_args[0], "disk")) {
		if (!(*log_health = dm_strdup(log_args[2]))) {
			log_error("Allocation of log string failed.");
			return 0;
		}
		if (sscanf(log_args[1], "%d:%d", &major, &minor) != 2) {
			log_error("Failed to parse log's device number from %s.", log_args[1]);
			goto out;
		}
		*log_dev = MKDEV((dev_t)major, minor);
	}

	if (!(*images_health = dm_strdup(args[2 + num_devs]))) {
		log_error("Allocation of images string failed.");
		goto out;
	}

	return 1;

out:
	dm_free(*log_health);
	*log_health = NULL;
	*log_dev = 0;

	return 0;
}

/*
 * ignore_blocked_mirror_devices
 * @dev
 * @start
 * @length
 * @mirror_status_str
 *
 * When a DM 'mirror' target is created with 'block_on_error' or
 * 'handle_errors', it will block I/O if there is a device failure
 * until the mirror is reconfigured.  Thus, LVM should never attempt
 * to read labels from a mirror that has a failed device.  (LVM
 * commands are issued to repair mirrors; and if LVM is blocked
 * attempting to read a mirror, a circular dependency would be created.)
 *
 * This function is a slimmed-down version of lib/mirror/mirrored.c:
 * _mirrored_transient_status().
 *
 * If a failed device is detected in the status string, then it must be
 * determined if 'block_on_error' or 'handle_errors' was used when
 * creating the mirror.  This info can only be determined from the mirror
 * table.  The 'dev', 'start', 'length' trio allow us to correlate the
 * 'mirror_status_str' with the correct device table in order to check
 * for blocking.
 *
 * Returns: 1 if mirror should be ignored, 0 if safe to use
 */
static int _ignore_blocked_mirror_devices(struct device *dev,
					  uint64_t start, uint64_t length,
					  char *mirror_status_str)
{
	unsigned i, check_for_blocking = 0;
	dev_t log_dev;
	char *images_health, *log_health;
	uint64_t s,l;
	char *p, *params, *target_type = NULL;
	void *next = NULL;
	struct dm_task *dmt = NULL;
	int r = 0;

	if (!_parse_mirror_status(mirror_status_str,
				  &images_health, &log_dev, &log_health))
		return_0;

	for (i = 0; images_health[i]; i++)
		if (images_health[i] != 'A') {
			log_debug_activation("%s: Mirror image %d marked as failed",
					     dev_name(dev), i);
			check_for_blocking = 1;
		}

	if (!check_for_blocking && log_dev) {
		if (log_health[0] != 'A') {
			log_debug_activation("%s: Mirror log device marked as failed",
					     dev_name(dev));
			check_for_blocking = 1;
		} else {
			struct device *tmp_dev;
			char buf[16];

			if (dm_snprintf(buf, sizeof(buf), "%d:%d",
					(int)MAJOR(log_dev),
					(int)MINOR(log_dev)) < 0)
				goto_out;

			if (!(tmp_dev = dev_create_file(buf, NULL, NULL, 0)))
				goto_out;

			tmp_dev->dev = log_dev;
			if (device_is_suspended_or_blocking(tmp_dev))
				goto_out;
		}
	}

	if (!check_for_blocking) {
		r = 1;
		goto out;
	}

	/*
	 * We avoid another system call if we can, but if a device is
	 * dead, we have no choice but to look up the table too.
	 */
	if (!(dmt = dm_task_create(DM_DEVICE_TABLE)))
		goto_out;

	if (!dm_task_set_major_minor(dmt, MAJOR(dev->dev), MINOR(dev->dev), 1))
		goto_out;

	if (activation_checks() && !dm_task_enable_checks(dmt))
		goto_out;

	if (!dm_task_run(dmt))
		goto_out;

	do {
		next = dm_get_next_target(dmt, next, &s, &l,
					  &target_type, &params);
		if ((s == start) && (l == length)) {
			if (strcmp(target_type, "mirror"))
				goto_out;

			if (((p = strstr(params, " block_on_error")) &&
			     (p[15] == '\0' || p[15] == ' ')) ||
			    ((p = strstr(params, " handle_errors")) &&
			     (p[14] == '\0' || p[14] == ' '))) {
				log_debug_activation("%s: I/O blocked to mirror device",
						     dev_name(dev));
				goto out;
			}
		}
	} while (next);

	r = 1;
out:
	if (dmt)
		dm_task_destroy(dmt);
	dm_free(log_health);
	dm_free(images_health);

	return r;
}

/*
 * _device_is_usable
 * @dev
 * @check_lv_names
 *
 * A device is considered not usable if it is:
 *     1) An empty device (no targets)
 *     2) A blocked mirror (i.e. a mirror with a failure and block_on_error set)
 *     3) ignore_suspended_devices is set and
 *        a) the device is suspended
 *        b) it is a snapshot origin
 *     4) an error target
 * And optionally, if 'check_lv_names' is set
 *     5) the LV name is a reserved name.
 *
 * Returns: 1 if usable, 0 otherwise
 */
static int _device_is_usable(struct device *dev, int check_lv_names)
{
	struct dm_task *dmt;
	struct dm_info info;
	const char *name, *uuid;
	uint64_t start, length;
	char *target_type = NULL;
	char *params, *vgname = NULL, *lvname, *layer;
	void *next = NULL;
	int only_error_target = 1;
	int r = 0;

	if (!(dmt = dm_task_create(DM_DEVICE_STATUS)))
		return_0;

	if (!dm_task_set_major_minor(dmt, MAJOR(dev->dev), MINOR(dev->dev), 1))
		goto_out;

	if (activation_checks() && !dm_task_enable_checks(dmt))
		goto_out;
		
	if (!dm_task_run(dmt)) {
		log_error("Failed to get state of mapped device");
		goto out;
	}

	if (!dm_task_get_info(dmt, &info))
		goto_out;

	if (!info.exists)
		goto out;

	name = dm_task_get_name(dmt);
	uuid = dm_task_get_uuid(dmt);

	if (!info.target_count) {
		log_debug_activation("%s: Empty device %s not usable.", dev_name(dev), name);
		goto out;
	}

	if (info.suspended && ignore_suspended_devices()) {
		log_debug_activation("%s: Suspended device %s not usable.", dev_name(dev), name);
		goto out;
	}

	/* FIXME Also check for mpath no paths */
	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);

		if (target_type && !strcmp(target_type, "mirror") &&
		    !_ignore_blocked_mirror_devices(dev, start, length, params)) {
			log_debug_activation("%s: Mirror device %s not usable.",
					     dev_name(dev), name);
			goto out;
		}

		/*
		 * Snapshot origin could be sitting on top of a mirror which
		 * could be blocking I/O.  Skip snapshot origins entirely for
		 * now.
		 *
		 * FIXME: rather than skipping origin, check if mirror is
		 * underneath and if the mirror is blocking I/O.
		 */
		if (target_type && !strcmp(target_type, "snapshot-origin") &&
		    ignore_suspended_devices()) {
			log_debug_activation("%s: Snapshot-origin device %s not usable.",
					     dev_name(dev), name);
			goto out;
		}

		if (target_type && strcmp(target_type, "error"))
			only_error_target = 0;
	} while (next);

	/* Skip devices consisting entirely of error targets. */
	/* FIXME Deal with device stacked above error targets? */
	if (only_error_target) {
		log_debug_activation("%s: Error device %s not usable.",
				     dev_name(dev), name);
		goto out;
	}

	/* FIXME Also check dependencies? */

	/* Check internal lvm devices */
	if (check_lv_names &&
	    uuid && !strncmp(uuid, UUID_PREFIX, sizeof(UUID_PREFIX) - 1)) {
		if (!(vgname = dm_strdup(name)) ||
		    !dm_split_lvm_name(NULL, NULL, &vgname, &lvname, &layer))
			goto_out;

		if (lvname && (is_reserved_lvname(lvname) || *layer)) {
			log_debug_activation("%s: Reserved internal LV device %s/%s%s%s not usable.",
					     dev_name(dev), vgname, lvname, *layer ? "-" : "", layer);
			goto out;
		}
	}

	r = 1;

      out:
	dm_free(vgname);
	dm_task_destroy(dmt);
	return r;
}

int device_is_usable(struct device *dev)
{
	return _device_is_usable(dev, 1);
}

int device_is_suspended_or_blocking(struct device *dev)
{
	return !_device_is_usable(dev, 0);
}

static int _info(const char *dlid, int with_open_count, int with_read_ahead,
		 struct dm_info *info, uint32_t *read_ahead)
{
	int r = 0;

	if ((r = _info_run(NULL, dlid, info, read_ahead, 0, with_open_count,
			   with_read_ahead, 0, 0)) && info->exists)
		return 1;
	else if ((r = _info_run(NULL, dlid + sizeof(UUID_PREFIX) - 1, info,
				read_ahead, 0, with_open_count,
				with_read_ahead, 0, 0)) && info->exists)
		return 1;

	return r;
}

static int _info_by_dev(uint32_t major, uint32_t minor, struct dm_info *info)
{
	return _info_run(NULL, NULL, info, NULL, 0, 0, 0, major, minor);
}

int dev_manager_info(struct dm_pool *mem, const struct logical_volume *lv,
		     const char *layer,
		     int with_open_count, int with_read_ahead,
		     struct dm_info *info, uint32_t *read_ahead)
{
	char *dlid, *name;
	int r;

	if (!(name = dm_build_dm_name(mem, lv->vg->name, lv->name, layer))) {
		log_error("name build failed for %s", lv->name);
		return 0;
	}

	if (!(dlid = build_dm_uuid(mem, lv->lvid.s, layer))) {
		log_error("dlid build failed for %s", name);
		r = 0;
		goto out;
	}

	log_debug_activation("Getting device info for %s [%s]", name, dlid);
	r = _info(dlid, with_open_count, with_read_ahead, info, read_ahead);
out:
	dm_pool_free(mem, name);

	return r;
}

static const struct dm_info *_cached_info(struct dm_pool *mem,
					  struct dm_tree *dtree,
					  const struct logical_volume *lv,
					  const char *layer)
{
	char *dlid;
	const struct dm_tree_node *dnode;
	const struct dm_info *dinfo = NULL;

	if (!(dlid = build_dm_uuid(mem, lv->lvid.s, layer))) {
		log_error("Failed to build dlid for %s.", lv->name);
		return NULL;
	}

	if (!(dnode = dm_tree_find_node_by_uuid(dtree, dlid)))
		goto out;

	if (!(dinfo = dm_tree_node_get_info(dnode))) {
		log_error("Failed to get info from tree node for %s.", lv->name);
		goto out;
	}

	if (!dinfo->exists)
		dinfo = NULL;
out:
	dm_pool_free(mem, dlid);

	return dinfo;
}

#if 0
/* FIXME Interface must cope with multiple targets */
static int _status_run(const char *name, const char *uuid,
		       unsigned long long *s, unsigned long long *l,
		       char **t, uint32_t t_size, char **p, uint32_t p_size)
{
	int r = 0;
	struct dm_task *dmt;
	struct dm_info info;
	void *next = NULL;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;

	if (!(dmt = _setup_task(name, uuid, 0, DM_DEVICE_STATUS, 0, 0)))
		return_0;

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!dm_task_run(dmt))
		goto_out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto_out;

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &type, &params);
		if (type) {
			*s = start;
			*l = length;
			/* Make sure things are null terminated */
			strncpy(*t, type, t_size);
			(*t)[t_size - 1] = '\0';
			strncpy(*p, params, p_size);
			(*p)[p_size - 1] = '\0';

			r = 1;
			/* FIXME Cope with multiple targets! */
			break;
		}

	} while (next);

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _status(const char *name, const char *uuid,
		   unsigned long long *start, unsigned long long *length,
		   char **type, uint32_t type_size, char **params,
		   uint32_t param_size) __attribute__ ((unused));

static int _status(const char *name, const char *uuid,
		   unsigned long long *start, unsigned long long *length,
		   char **type, uint32_t type_size, char **params,
		   uint32_t param_size)
{
	if (uuid && *uuid) {
		if (_status_run(NULL, uuid, start, length, type,
				type_size, params, param_size) &&
		    *params)
			return 1;
		else if (_status_run(NULL, uuid + sizeof(UUID_PREFIX) - 1, start,
				     length, type, type_size, params,
				     param_size) &&
			 *params)
			return 1;
	}

	if (name && _status_run(name, NULL, start, length, type, type_size,
				params, param_size))
		return 1;

	return 0;
}
#endif

int lv_has_target_type(struct dm_pool *mem, struct logical_volume *lv,
		       const char *layer, const char *target_type)
{
	int r = 0;
	char *dlid;
	struct dm_task *dmt;
	struct dm_info info;
	void *next = NULL;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;

	if (!(dlid = build_dm_uuid(mem, lv->lvid.s, layer)))
		return_0;

	if (!(dmt = _setup_task(NULL, dlid, 0,
				DM_DEVICE_STATUS, 0, 0)))
		goto_bad;

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!dm_task_run(dmt))
		goto_out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto_out;

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &type, &params);
		if (type && strncmp(type, target_type,
				    strlen(target_type)) == 0) {
			if (info.live_table)
				r = 1;
			break;
		}
	} while (next);

out:
	dm_task_destroy(dmt);
bad:
	dm_pool_free(mem, dlid);

	return r;
}

int add_linear_area_to_dtree(struct dm_tree_node *node, uint64_t size, uint32_t extent_size, int use_linear_target, const char *vgname, const char *lvname)
{
	uint32_t page_size;

	/*
	 * Use striped or linear target?
	 */
	if (!use_linear_target) {
		page_size = lvm_getpagesize() >> SECTOR_SHIFT;

		/*
		 * We'll use the extent size as the stripe size.
		 * Extent size and page size are always powers of 2.
		 * The striped target requires that the stripe size is
		 * divisible by the page size.
		 */
		if (extent_size >= page_size) {
			/* Use striped target */
			if (!dm_tree_node_add_striped_target(node, size, extent_size))
				return_0;
			return 1;
		} else
			/* Some exotic cases are unsupported by striped. */
			log_warn("WARNING: Using linear target for %s/%s: Striped requires extent size (%" PRIu32 " sectors) >= page size (%" PRIu32 ").",
				 vgname, lvname, extent_size, page_size);
	}

	/*
	 * Use linear target.
	 */
	if (!dm_tree_node_add_linear_target(node, size))
		return_0;

	return 1;
}

static percent_range_t _combine_percent(percent_t a, percent_t b,
                                        uint32_t numerator, uint32_t denominator)
{
	if (a == PERCENT_MERGE_FAILED || b == PERCENT_MERGE_FAILED)
		return PERCENT_MERGE_FAILED;

	if (a == PERCENT_INVALID || b == PERCENT_INVALID)
		return PERCENT_INVALID;

	if (a == PERCENT_100 && b == PERCENT_100)
		return PERCENT_100;

	if (a == PERCENT_0 && b == PERCENT_0)
		return PERCENT_0;

	return (percent_range_t) make_percent(numerator, denominator);
}

static int _percent_run(struct dev_manager *dm, const char *name,
			const char *dlid,
			const char *target_type, int wait,
			const struct logical_volume *lv, percent_t *overall_percent,
			uint32_t *event_nr, int fail_if_percent_unsupported)
{
	int r = 0;
	struct dm_task *dmt;
	struct dm_info info;
	void *next = NULL;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;
	const struct dm_list *segh = lv ? &lv->segments : NULL;
	struct lv_segment *seg = NULL;
	struct segment_type *segtype;
	int first_time = 1;
	percent_t percent = PERCENT_INVALID;

	uint64_t total_numerator = 0, total_denominator = 0;

	*overall_percent = percent;

	if (!(dmt = _setup_task(name, dlid, event_nr,
				wait ? DM_DEVICE_WAITEVENT : DM_DEVICE_STATUS, 0, 0)))
		return_0;

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!dm_task_run(dmt))
		goto_out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto_out;

	if (event_nr)
		*event_nr = info.event_nr;

	do {
		next = dm_get_next_target(dmt, next, &start, &length, &type,
					  &params);
		if (lv) {
			if (!(segh = dm_list_next(&lv->segments, segh))) {
				log_error("Number of segments in active LV %s "
					  "does not match metadata", lv->name);
				goto out;
			}
			seg = dm_list_item(segh, struct lv_segment);
		}

		if (!type || !params)
			continue;

		if (!(segtype = get_segtype_from_string(dm->cmd, target_type)))
			continue;

		if (strcmp(type, target_type)) {
			/* If kernel's type isn't an exact match is it compatible? */
			if (!segtype->ops->target_status_compatible ||
			    !segtype->ops->target_status_compatible(type))
				continue;
		}

		if (!segtype->ops->target_percent)
			continue;

		if (!segtype->ops->target_percent(&dm->target_state,
						  &percent, dm->mem,
						  dm->cmd, seg, params,
						  &total_numerator,
						  &total_denominator))
			goto_out;

		if (first_time) {
			*overall_percent = percent;
			first_time = 0;
		} else
			*overall_percent =
				_combine_percent(*overall_percent, percent,
						 total_numerator, total_denominator);
	} while (next);

	if (lv && dm_list_next(&lv->segments, segh)) {
		log_error("Number of segments in active LV %s does not "
			  "match metadata", lv->name);
		goto out;
	}

	if (first_time) {
		/* above ->target_percent() was not executed! */
		/* FIXME why return PERCENT_100 et. al. in this case? */
		*overall_percent = PERCENT_100;
		if (fail_if_percent_unsupported)
			goto_out;
	}

	log_debug_activation("LV percent: %f", percent_to_float(*overall_percent));
	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _percent(struct dev_manager *dm, const char *name, const char *dlid,
		    const char *target_type, int wait,
		    const struct logical_volume *lv, percent_t *percent,
		    uint32_t *event_nr, int fail_if_percent_unsupported)
{
	if (dlid && *dlid) {
		if (_percent_run(dm, NULL, dlid, target_type, wait, lv, percent,
				 event_nr, fail_if_percent_unsupported))
			return 1;
		else if (_percent_run(dm, NULL, dlid + sizeof(UUID_PREFIX) - 1,
				      target_type, wait, lv, percent,
				      event_nr, fail_if_percent_unsupported))
			return 1;
	}

	if (name && _percent_run(dm, name, NULL, target_type, wait, lv, percent,
				 event_nr, fail_if_percent_unsupported))
		return 1;

	return_0;
}

/* FIXME Merge with the percent function */
int dev_manager_transient(struct dev_manager *dm, struct logical_volume *lv)
{
	int r = 0;
	struct dm_task *dmt;
	struct dm_info info;
	void *next = NULL;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;
	char *dlid = NULL;
	const char *layer = lv_layer(lv);
	const struct dm_list *segh = &lv->segments;
	struct lv_segment *seg = NULL;

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, layer)))
		return_0;

	if (!(dmt = _setup_task(0, dlid, NULL, DM_DEVICE_STATUS, 0, 0)))
		return_0;

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!dm_task_run(dmt))
		goto_out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto_out;

	do {
		next = dm_get_next_target(dmt, next, &start, &length, &type,
					  &params);

		if (!(segh = dm_list_next(&lv->segments, segh))) {
		    log_error("Number of segments in active LV %s "
			      "does not match metadata", lv->name);
		    goto out;
		}
		seg = dm_list_item(segh, struct lv_segment);

		if (!type || !params)
			continue;

		if (!seg) {
			log_error(INTERNAL_ERROR "Segment is not selected.");
			goto out;
		}

		if (seg->segtype->ops->check_transient_status &&
		    !seg->segtype->ops->check_transient_status(seg, params))
			goto_out;

	} while (next);

	if (dm_list_next(&lv->segments, segh)) {
		log_error("Number of segments in active LV %s does not "
			  "match metadata", lv->name);
		goto out;
	}

	r = 1;

      out:
	dm_task_destroy(dmt);
	return r;
}

/*
 * dev_manager implementation.
 */
struct dev_manager *dev_manager_create(struct cmd_context *cmd,
				       const char *vg_name,
				       unsigned track_pvmove_deps)
{
	struct dm_pool *mem;
	struct dev_manager *dm;

	if (!(mem = dm_pool_create("dev_manager", 16 * 1024)))
		return_NULL;

	if (!(dm = dm_pool_zalloc(mem, sizeof(*dm))))
		goto_bad;

	dm->cmd = cmd;
	dm->mem = mem;

	if (!(dm->vg_name = dm_pool_strdup(dm->mem, vg_name)))
		goto_bad;

	/*
	 * When we manipulate (normally suspend/resume) the PVMOVE
	 * device directly, there's no need to touch the LVs above.
	 */
	dm->track_pvmove_deps = track_pvmove_deps;

	dm->target_state = NULL;

	dm_udev_set_sync_support(cmd->current_settings.udev_sync);

	return dm;

      bad:
	dm_pool_destroy(mem);
	return NULL;
}

void dev_manager_destroy(struct dev_manager *dm)
{
	dm_pool_destroy(dm->mem);
}

void dev_manager_release(void)
{
	dm_lib_release();
}

void dev_manager_exit(void)
{
	dm_lib_exit();
}

int dev_manager_snapshot_percent(struct dev_manager *dm,
				 const struct logical_volume *lv,
				 percent_t *percent)
{
	const struct logical_volume *snap_lv;
	char *name;
	const char *dlid;
	int fail_if_percent_unsupported = 0;

	if (lv_is_merging_origin(lv)) {
		/*
		 * Set 'fail_if_percent_unsupported', otherwise passing
		 * unsupported LV types to _percent will lead to a default
		 * successful return with percent_range as PERCENT_100.
		 * - For a merging origin, this will result in a polldaemon
		 *   that runs infinitely (because completion is PERCENT_0)
		 * - We unfortunately don't yet _know_ if a snapshot-merge
		 *   target is active (activation is deferred if dev is open);
		 *   so we can't short-circuit origin devices based purely on
		 *   existing LVM LV attributes.
		 */
		fail_if_percent_unsupported = 1;
	}

	if (lv_is_merging_cow(lv)) {
		/* must check percent of origin for a merging snapshot */
		snap_lv = origin_from_cow(lv);
	} else
		snap_lv = lv;

	/*
	 * Build a name for the top layer.
	 */
	if (!(name = dm_build_dm_name(dm->mem, snap_lv->vg->name, snap_lv->name, NULL)))
		return_0;

	if (!(dlid = build_dm_uuid(dm->mem, snap_lv->lvid.s, NULL)))
		return_0;

	/*
	 * Try and get some info on this device.
	 */
	log_debug_activation("Getting device status percentage for %s", name);
	if (!_percent(dm, name, dlid, "snapshot", 0, NULL, percent,
		      NULL, fail_if_percent_unsupported))
		return_0;

	/* If the snapshot isn't available, percent will be -1 */
	return 1;
}

/* FIXME Merge with snapshot_percent, auto-detecting target type */
/* FIXME Cope with more than one target */
int dev_manager_mirror_percent(struct dev_manager *dm,
			       const struct logical_volume *lv, int wait,
			       percent_t *percent, uint32_t *event_nr)
{
	char *name;
	const char *dlid;
	const char *target_type = first_seg(lv)->segtype->name;
	const char *layer = lv_layer(lv);

	/*
	 * Build a name for the top layer.
	 */
	if (!(name = dm_build_dm_name(dm->mem, lv->vg->name, lv->name, layer)))
		return_0;

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, layer))) {
		log_error("dlid build failed for %s", lv->name);
		return 0;
	}

	log_debug_activation("Getting device %s status percentage for %s",
			     target_type, name);
	if (!_percent(dm, name, dlid, target_type, wait, lv, percent,
		      event_nr, 0))
		return_0;

	return 1;
}

int dev_manager_raid_status(struct dev_manager *dm,
			    const struct logical_volume *lv,
			    struct dm_status_raid **status)
{
	int r = 0;
	const char *dlid;
	struct dm_task *dmt;
	struct dm_info info;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;
	const char *layer = lv_layer(lv);

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, layer)))
		return_0;

	log_debug_activation("Getting raid device status for %s.", lv->name);

	if (!(dmt = _setup_task(NULL, dlid, 0, DM_DEVICE_STATUS, 0, 0)))
		return_0;

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count.");

	if (!dm_task_run(dmt))
		goto_out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto_out;

	dm_get_next_target(dmt, NULL, &start, &length, &type, &params);

	if (!type || strcmp(type, "raid")) {
		log_debug("Expected raid segment type but got %s instead",
			  type ? type : "NULL");
		goto out;
	}

	/* FIXME Check there's only one target */

	if (!dm_get_status_raid(dm->mem, params, status))
		goto_out;

	r = 1;
out:
	dm_task_destroy(dmt);

	return r;
}

int dev_manager_raid_message(struct dev_manager *dm,
			     const struct logical_volume *lv,
			     const char *msg)
{
	int r = 0;
	const char *dlid;
	struct dm_task *dmt;
	const char *layer = lv_layer(lv);

	if (!(lv->status & RAID)) {
		log_error(INTERNAL_ERROR "%s/%s is not a RAID logical volume",
			  lv->vg->name, lv->name);
		return 0;
	}

	/* These are the supported RAID messages for dm-raid v1.5.0 */
	if (!strcmp(msg, "idle") &&
	    !strcmp(msg, "frozen") &&
	    !strcmp(msg, "resync") &&
	    !strcmp(msg, "recover") &&
	    !strcmp(msg, "check") &&
	    !strcmp(msg, "repair") &&
	    !strcmp(msg, "reshape")) {
		log_error("Unknown RAID message: %s", msg);
		return 0;
	}

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, layer)))
		return_0;

	if (!(dmt = _setup_task(NULL, dlid, 0, DM_DEVICE_TARGET_MSG, 0, 0)))
		return_0;

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count.");

	if (!dm_task_set_message(dmt, msg))
		goto_out;

	if (!dm_task_run(dmt))
		goto_out;

	r = 1;
out:
	dm_task_destroy(dmt);

	return r;
}

#if 0
	log_very_verbose("%s %s", sus ? "Suspending" : "Resuming", name);

	log_verbose("Loading %s", dl->name);
			log_very_verbose("Activating %s read-only", dl->name);
	log_very_verbose("Activated %s %s %03u:%03u", dl->name,
			 dl->dlid, dl->info.major, dl->info.minor);

	if (_get_flag(dl, VISIBLE))
		log_verbose("Removing %s", dl->name);
	else
		log_very_verbose("Removing %s", dl->name);

	log_debug_activation("Adding target: %" PRIu64 " %" PRIu64 " %s %s",
		  extent_size * seg->le, extent_size * seg->len, target, params);

	log_debug_activation("Adding target: 0 %" PRIu64 " snapshot-origin %s",
		  dl->lv->size, params);
	log_debug_activation("Adding target: 0 %" PRIu64 " snapshot %s", size, params);
	log_debug_activation("Getting device info for %s", dl->name);

	/* Rename? */
		if ((suffix = strrchr(dl->dlid + sizeof(UUID_PREFIX) - 1, '-')))
			suffix++;
		new_name = dm_build_dm_name(dm->mem, dm->vg_name, dl->lv->name,
					suffix);

static int _belong_to_vg(const char *vgname, const char *name)
{
	const char *v = vgname, *n = name;

	while (*v) {
		if ((*v != *n) || (*v == '-' && *(++n) != '-'))
			return 0;
		v++, n++;
	}

	if (*n == '-' && *(n + 1) != '-')
		return 1;
	else
		return 0;
}

	if (!(snap_seg = find_snapshot(lv)))
		return 1;

	old_origin = snap_seg->origin;

	/* Was this the last active snapshot with this origin? */
	dm_list_iterate_items(lvl, active_head) {
		active = lvl->lv;
		if ((snap_seg = find_snapshot(active)) &&
		    snap_seg->origin == old_origin) {
			return 1;
		}
	}

#endif

int dev_manager_thin_pool_status(struct dev_manager *dm,
				 const struct logical_volume *lv,
				 struct dm_status_thin_pool **status,
				 int noflush)
{
	const char *dlid;
	struct dm_task *dmt;
	struct dm_info info;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;
	int r = 0;

	/* Build dlid for the thin pool layer */
	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, lv_layer(lv))))
		return_0;

	log_debug_activation("Getting thin pool device status for %s.", lv->name);

	if (!(dmt = _setup_task(NULL, dlid, 0, DM_DEVICE_STATUS, 0, 0)))
		return_0;

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count.");

	if (noflush && !dm_task_no_flush(dmt))
		log_warn("Can't set no_flush.");

	if (!dm_task_run(dmt))
		goto_out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto_out;

	dm_get_next_target(dmt, NULL, &start, &length, &type, &params);

	/* FIXME Check for thin and check there's exactly one target */

	if (!dm_get_status_thin_pool(dm->mem, params, status))
		goto_out;

	r = 1;
out:
	dm_task_destroy(dmt);

	return r;
}

int dev_manager_thin_pool_percent(struct dev_manager *dm,
				  const struct logical_volume *lv,
				  int metadata, percent_t *percent)
{
	char *name;
	const char *dlid;

	/* Build a name for the top layer */
	if (!(name = dm_build_dm_name(dm->mem, lv->vg->name, lv->name,
				      lv_layer(lv))))
		return_0;

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, lv_layer(lv))))
		return_0;

	log_debug_activation("Getting device status percentage for %s", name);
	if (!(_percent(dm, name, dlid, "thin-pool", 0,
		       (metadata) ? lv : NULL, percent, NULL, 1)))
		return_0;

	return 1;
}

int dev_manager_thin_percent(struct dev_manager *dm,
			     const struct logical_volume *lv,
			     int mapped, percent_t *percent)
{
	char *name;
	const char *dlid;
	const char *layer = lv_layer(lv);

	/* Build a name for the top layer */
	if (!(name = dm_build_dm_name(dm->mem, lv->vg->name, lv->name, layer)))
		return_0;

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, layer)))
		return_0;

	log_debug_activation("Getting device status percentage for %s", name);
	if (!(_percent(dm, name, dlid, "thin", 0,
		       (mapped) ? NULL : lv, percent, NULL, 1)))
		return_0;

	return 1;
}

/*************************/
/*  NEW CODE STARTS HERE */
/*************************/

static int _dev_manager_lv_mknodes(const struct logical_volume *lv)
{
	char *name;

	if (!(name = dm_build_dm_name(lv->vg->cmd->mem, lv->vg->name,
				   lv->name, NULL)))
		return_0;

	return fs_add_lv(lv, name);
}

static int _dev_manager_lv_rmnodes(const struct logical_volume *lv)
{
	return fs_del_lv(lv);
}

int dev_manager_mknodes(const struct logical_volume *lv)
{
	struct dm_info dminfo;
	char *name;
	int r = 0;

	if (!(name = dm_build_dm_name(lv->vg->cmd->mem, lv->vg->name, lv->name, NULL)))
		return_0;

	if ((r = _info_run(name, NULL, &dminfo, NULL, 1, 0, 0, 0, 0))) {
		if (dminfo.exists) {
			if (lv_is_visible(lv))
				r = _dev_manager_lv_mknodes(lv);
		} else
			r = _dev_manager_lv_rmnodes(lv);
	}

	dm_pool_free(lv->vg->cmd->mem, name);
	return r;
}

#ifdef UDEV_SYNC_SUPPORT
/*
 * Until the DM_UEVENT_GENERATED_FLAG was introduced in kernel patch
 * 856a6f1dbd8940e72755af145ebcd806408ecedd
 * some operations could not be performed by udev, requiring our fallback code.
 */
static int _dm_driver_has_stable_udev_support(void)
{
	char vsn[80];
	unsigned maj, min, patchlevel;

	return driver_version(vsn, sizeof(vsn)) &&
	       (sscanf(vsn, "%u.%u.%u", &maj, &min, &patchlevel) == 3) &&
	       (maj == 4 ? min >= 18 : maj > 4);
}

static int _check_udev_fallback(struct cmd_context *cmd)
{
	struct config_info *settings = &cmd->current_settings;

	if (settings->udev_fallback != -1)
		goto out;

	/*
	 * Use udev fallback automatically in case udev
	 * is disabled via DM_DISABLE_UDEV environment
	 * variable or udev rules are switched off.
	 */
	settings->udev_fallback = !settings->udev_rules ? 1 :
		find_config_tree_bool(cmd, activation_verify_udev_operations_CFG, NULL);

	/* Do not rely fully on udev if the udev support is known to be incomplete. */
	if (!settings->udev_fallback && !_dm_driver_has_stable_udev_support()) {
		log_very_verbose("Kernel driver has incomplete udev support so "
				 "LVM will check and perform some operations itself.");
		settings->udev_fallback = 1;
	}
out:
	return settings->udev_fallback;
}

#else /* UDEV_SYNC_SUPPORT */

static int _check_udev_fallback(struct cmd_context *cmd)
{
	/* We must use old node/symlink creation code if not compiled with udev support at all! */
	return cmd->current_settings.udev_fallback = 1;
}

#endif /* UDEV_SYNC_SUPPORT */

static uint16_t _get_udev_flags(struct dev_manager *dm, struct logical_volume *lv,
				const char *layer)
{
	uint16_t udev_flags = 0;

	/*
	 * Instruct also libdevmapper to disable udev
	 * fallback in accordance to LVM2 settings.
	 */
	if (!_check_udev_fallback(dm->cmd))
		udev_flags |= DM_UDEV_DISABLE_LIBRARY_FALLBACK;

	/*
	 * Is this top-level and visible device?
	 * If not, create just the /dev/mapper content.
	 */
	/* FIXME: add target's method for this */
	if (layer || !lv_is_visible(lv) || lv_is_thin_pool(lv))
		udev_flags |= DM_UDEV_DISABLE_SUBSYSTEM_RULES_FLAG |
			      DM_UDEV_DISABLE_DISK_RULES_FLAG |
			      DM_UDEV_DISABLE_OTHER_RULES_FLAG;
	/*
	 * There's no need for other udev rules to touch special LVs with
	 * reserved names. We don't need to populate /dev/disk here either.
	 * Even if they happen to be visible and top-level.
	 */
	else if (is_reserved_lvname(lv->name))
		udev_flags |= DM_UDEV_DISABLE_DISK_RULES_FLAG |
			      DM_UDEV_DISABLE_OTHER_RULES_FLAG;

	/*
	 * Snapshots and origins could have the same rule applied that will
	 * give symlinks exactly the same name (e.g. a name based on
	 * filesystem UUID). We give preference to origins to make such
	 * naming deterministic (e.g. symlinks in /dev/disk/by-uuid).
	 */
	if (lv_is_cow(lv))
		udev_flags |= DM_UDEV_LOW_PRIORITY_FLAG;

	/*
	 * Finally, add flags to disable /dev/mapper and /dev/<vgname> content
	 * to be created by udev if it is requested by user's configuration.
	 * This is basically an explicit fallback to old node/symlink creation
	 * without udev.
	 */
	if (!dm->cmd->current_settings.udev_rules)
		udev_flags |= DM_UDEV_DISABLE_DM_RULES_FLAG |
			      DM_UDEV_DISABLE_SUBSYSTEM_RULES_FLAG;

	return udev_flags;
}

static int _add_dev_to_dtree(struct dev_manager *dm, struct dm_tree *dtree,
			     struct logical_volume *lv, const char *layer)
{
	char *dlid, *name;
	struct dm_info info, info2;

	if (!(name = dm_build_dm_name(dm->mem, lv->vg->name, lv->name, layer)))
		return_0;

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, layer)))
		return_0;

	log_debug_activation("Getting device info for %s [%s]", name, dlid);
	if (!_info(dlid, 1, 0, &info, NULL)) {
		log_error("Failed to get info for %s [%s].", name, dlid);
		return 0;
	}

	/*
	 * For top level volumes verify that existing device match
	 * requested major/minor and that major/minor pair is available for use
	 */
	if (!layer && lv->major != -1 && lv->minor != -1) {
		/*
		 * FIXME compare info.major with lv->major if multiple major support
		 */
		if (info.exists && (info.minor != lv->minor)) {
			log_error("Volume %s (%" PRIu32 ":%" PRIu32")"
				  " differs from already active device "
				  "(%" PRIu32 ":%" PRIu32")",
				  lv->name, lv->major, lv->minor, info.major, info.minor);
			return 0;
		}
		if (!info.exists && _info_by_dev(lv->major, lv->minor, &info2) &&
		    info2.exists) {
			log_error("The requested major:minor pair "
				  "(%" PRIu32 ":%" PRIu32") is already used",
				  lv->major, lv->minor);
			return 0;
		}
	}

	if (info.exists && !dm_tree_add_dev_with_udev_flags(dtree, info.major, info.minor,
							_get_udev_flags(dm, lv, layer))) {
		log_error("Failed to add device (%" PRIu32 ":%" PRIu32") to dtree",
			  info.major, info.minor);
		return 0;
	}

	return 1;
}

/*
 * Add replicator devices
 *
 * Using _add_dev_to_dtree() directly instead of _add_lv_to_dtree()
 * to avoid extra checks with extensions.
 */
static int _add_partial_replicator_to_dtree(struct dev_manager *dm,
					    struct dm_tree *dtree,
					    struct logical_volume *lv)
{
	struct logical_volume *rlv = first_seg(lv)->replicator;
	struct replicator_device *rdev;
	struct replicator_site *rsite;
	struct dm_tree_node *rep_node, *rdev_node;
	const char *uuid;

	if (!lv_is_active_replicator_dev(lv)) {
		if (!_add_dev_to_dtree(dm, dtree, lv->rdevice->lv,
				      NULL))
			return_0;
		return 1;
	}

	/* Add _rlog and replicator device */
	if (!_add_dev_to_dtree(dm, dtree, first_seg(rlv)->rlog_lv, NULL))
		return_0;

	if (!_add_dev_to_dtree(dm, dtree, rlv, NULL))
		return_0;

	if (!(uuid = build_dm_uuid(dm->mem, rlv->lvid.s, NULL)))
		return_0;

	rep_node = dm_tree_find_node_by_uuid(dtree, uuid);

	/* Add all related devices for replicator */
	dm_list_iterate_items(rsite, &rlv->rsites)
		dm_list_iterate_items(rdev, &rsite->rdevices) {
			if (rsite->state == REPLICATOR_STATE_ACTIVE) {
				/* Add _rimage LV */
				if (!_add_dev_to_dtree(dm, dtree, rdev->lv, NULL))
					return_0;

				/* Add replicator-dev LV, except of the already added one */
				if ((lv != rdev->replicator_dev->lv) &&
				    !_add_dev_to_dtree(dm, dtree,
						       rdev->replicator_dev->lv, NULL))
					return_0;

				/* If replicator exists - try connect existing heads */
				if (rep_node) {
					uuid = build_dm_uuid(dm->mem,
							     rdev->replicator_dev->lv->lvid.s,
							     NULL);
					if (!uuid)
						return_0;

					rdev_node = dm_tree_find_node_by_uuid(dtree, uuid);
					if (rdev_node)
						dm_tree_node_set_presuspend_node(rdev_node,
										 rep_node);
				}
			}

			if (!rdev->rsite->vg_name)
				continue;

			if (!_add_dev_to_dtree(dm, dtree, rdev->lv, NULL))
				return_0;

			if (rdev->slog &&
			    !_add_dev_to_dtree(dm, dtree, rdev->slog, NULL))
				return_0;
		}

	return 1;
}

struct thin_cb_data {
	const struct logical_volume *pool_lv;
	struct dev_manager *dm;
};

static int _thin_pool_callback(struct dm_tree_node *node,
			       dm_node_callback_t type, void *cb_data)
{
	int ret, status;
	const struct thin_cb_data *data = cb_data;
	const char *dmdir = dm_dir();
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	const char *thin_check =
		find_config_tree_str_allow_empty(data->pool_lv->vg->cmd, global_thin_check_executable_CFG, NULL);
	const struct logical_volume *mlv = first_seg(data->pool_lv)->metadata_lv;
	size_t len = strlen(dmdir) + 2 * (strlen(mlv->vg->name) + strlen(mlv->name)) + 3;
	char meta_path[len];
	int args = 0;
	const char *argv[19]; /* Max supported 15 args */
	char *split, *dm_name;

	if (!thin_check[0])
		return 1; /* Checking disabled */

	if (!(dm_name = dm_build_dm_name(data->dm->mem, mlv->vg->name,
					 mlv->name, NULL)) ||
	    (dm_snprintf(meta_path, len, "%s/%s", dmdir, dm_name) < 0)) {
		log_error("Failed to build thin metadata path.");
		return 0;
	}

	if ((cn = find_config_tree_node(mlv->vg->cmd, global_thin_check_options_CFG, NULL))) {
		for (cv = cn->v; cv && args < 16; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Invalid string in config file: "
					  "global/thin_check_options");
				return 0;
			}
			argv[++args] = cv->v.str;
		}
	} else {
		/* Use default options (no support for options with spaces) */
		if (!(split = dm_pool_strdup(data->dm->mem, DEFAULT_THIN_CHECK_OPTIONS))) {
			log_error("Failed to duplicate thin check string.");
			return 0;
		}
		args = dm_split_words(split, 16, 0, (char**) argv + 1);
	}

	if (args == 16) {
		log_error("Too many options for thin check command.");
		return 0;
	}

	argv[0] = thin_check;
	argv[++args] = meta_path;
	argv[++args] = NULL;

	if (!(ret = exec_cmd(data->pool_lv->vg->cmd, (const char * const *)argv,
			     &status, 0))) {
		switch (type) {
		case DM_NODE_CALLBACK_PRELOADED:
			log_err_once("Check of thin pool %s/%s failed (status:%d). "
				     "Manual repair required (thin_dump --repair %s)!",
				     data->pool_lv->vg->name, data->pool_lv->name,
				     status, meta_path);
			break;
		default:
			log_warn("WARNING: Integrity check of metadata for thin pool "
				 "%s/%s failed.",
				 data->pool_lv->vg->name, data->pool_lv->name);
		}
		/*
		 * FIXME: What should we do here??
		 *
		 * Maybe mark the node, so it's not activating
		 * as thin_pool but as error/linear and let the
		 * dm tree resolve the issue.
		 */
	}

	dm_pool_free(data->dm->mem, dm_name);

	return ret;
}

static int _thin_pool_register_callback(struct dev_manager *dm,
					struct dm_tree_node *node,
					const struct logical_volume *lv)
{
	struct thin_cb_data *data;

	/* Skip metadata testing for unused pool. */
	if (!first_seg(lv)->transaction_id)
		return 1;

	if (!(data = dm_pool_alloc(dm->mem, sizeof(*data)))) {
		log_error("Failed to allocated path for callback.");
		return 0;
	}

	data->dm = dm;
	data->pool_lv = lv;

	dm_tree_node_set_callback(node, _thin_pool_callback, data);

	return 1;
}

/*
 * Add LV and any known dependencies
 */
static int _add_lv_to_dtree(struct dev_manager *dm, struct dm_tree *dtree,
			    struct logical_volume *lv, int origin_only)
{
	uint32_t s;
	struct seg_list *sl;
	struct dm_list *snh;
	struct lv_segment *seg;
	struct dm_tree_node *thin_node;
	const char *uuid;

	if (!origin_only && !_add_dev_to_dtree(dm, dtree, lv, NULL))
		return_0;

	/* FIXME Can we avoid doing this every time? */
	/* Reused also for lv_is_external_origin(lv) */
	if (!_add_dev_to_dtree(dm, dtree, lv, "real"))
		return_0;

	if (!origin_only && !_add_dev_to_dtree(dm, dtree, lv, "cow"))
		return_0;

	if (origin_only && lv_is_thin_volume(lv)) {
		if (!_add_dev_to_dtree(dm, dtree, lv, lv_layer(lv)))
			return_0;
#if 0
		/* ? Use origin_only to avoid 'deep' thin pool suspend ? */
		/* FIXME Implement dm_tree_node_skip_childrens optimisation */
		if (!(uuid = build_dm_uuid(dm->mem, lv->lvid.s, lv_layer(lv))))
			return_0;
		if ((thin_node = dm_tree_find_node_by_uuid(dtree, uuid)))
			dm_tree_node_skip_childrens(thin_node, 1);
#endif
	}

	if (origin_only && dm->activation && !dm->skip_external_lv &&
	    lv_is_external_origin(lv)) {
		/* Find possible users of external origin lv */
		dm->skip_external_lv = 1; /* avoid recursion */
		dm_list_iterate_items(sl, &lv->segs_using_this_lv)
			/* Match only external_lv users */
			if ((sl->seg->external_lv == lv) &&
			    !_add_lv_to_dtree(dm, dtree, sl->seg->lv, 1))
				return_0;
		dm->skip_external_lv = 0;
	}

	if (lv_is_thin_pool(lv)) {
		/*
		 * For both origin_only and !origin_only
		 * skips test for -tpool-real and tpool-cow
		 */
		if (!_add_dev_to_dtree(dm, dtree, lv, lv_layer(lv)))
			return_0;
		if (!dm->activation) {
			/* Setup callback for non-activation partial tree */
			/* Activation gets own callback when needed */
			/* TODO: extend _cached_info() to return dnode */
			if (!(uuid = build_dm_uuid(dm->mem, lv->lvid.s, lv_layer(lv))))
				return_0;
			if ((thin_node = dm_tree_find_node_by_uuid(dtree, uuid)) &&
			    !_thin_pool_register_callback(dm, thin_node, lv))
				return_0;
		}
	}

	/* Add any snapshots of this LV */
	if (!origin_only && lv_is_origin(lv))
		dm_list_iterate(snh, &lv->snapshot_segs)
			if (!_add_lv_to_dtree(dm, dtree, dm_list_struct_base(snh, struct lv_segment, origin_list)->cow, 0))
				return_0;

	/* Add any LVs referencing a PVMOVE LV unless told not to. */
	if (dm->track_pvmove_deps && lv->status & PVMOVE) {
		dm->track_pvmove_deps = 0;
		dm_list_iterate_items(sl, &lv->segs_using_this_lv)
			if (!_add_lv_to_dtree(dm, dtree, sl->seg->lv, origin_only))
				return_0;
		dm->track_pvmove_deps = 1;
	}

	/* Adding LV head of replicator adds all other related devs */
	if (lv_is_replicator_dev(lv) &&
	    !_add_partial_replicator_to_dtree(dm, dtree, lv))
		return_0;

	/* Add any LVs used by segments in this LV */
	dm_list_iterate_items(seg, &lv->segments) {
		if (seg->external_lv && !dm->skip_external_lv &&
		    !_add_lv_to_dtree(dm, dtree, seg->external_lv, 1)) /* stack */
			return_0;
		if (seg->log_lv &&
		    !_add_lv_to_dtree(dm, dtree, seg->log_lv, 0))
			return_0;
		if (seg->metadata_lv &&
		    !_add_lv_to_dtree(dm, dtree, seg->metadata_lv, 0))
			return_0;
		if (seg->pool_lv && !dm->skip_external_lv &&
		    !_add_lv_to_dtree(dm, dtree, seg->pool_lv, 1)) /* stack */
			return_0;

		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) == AREA_LV && seg_lv(seg, s) &&
			    !_add_lv_to_dtree(dm, dtree, seg_lv(seg, s), 0))
				return_0;
			if (seg_is_raid(seg) &&
			    !_add_lv_to_dtree(dm, dtree, seg_metalv(seg, s), 0))
				return_0;
		}
	}

	return 1;
}

static struct dm_tree *_create_partial_dtree(struct dev_manager *dm, struct logical_volume *lv, int origin_only)
{
	struct dm_tree *dtree;

	if (!(dtree = dm_tree_create())) {
		log_debug_activation("Partial dtree creation failed for %s.", lv->name);
		return NULL;
	}

	if (!_add_lv_to_dtree(dm, dtree, lv, (lv_is_origin(lv) || lv_is_thin_volume(lv)) ? origin_only : 0))
		goto_bad;

	return dtree;

bad:
	dm_tree_free(dtree);
	return NULL;
}

static char *_add_error_device(struct dev_manager *dm, struct dm_tree *dtree,
			       struct lv_segment *seg, int s)
{
	char *dlid, *name;
	char errid[32];
	struct dm_tree_node *node;
	struct lv_segment *seg_i;
	struct dm_info info;
	int segno = -1, i = 0;
	uint64_t size = (uint64_t) seg->len * seg->lv->vg->extent_size;

	dm_list_iterate_items(seg_i, &seg->lv->segments) {
		if (seg == seg_i)
			segno = i;
		++i;
	}

	if (segno < 0) {
		log_error("_add_error_device called with bad segment");
		return NULL;
	}

	sprintf(errid, "missing_%d_%d", segno, s);

	if (!(dlid = build_dm_uuid(dm->mem, seg->lv->lvid.s, errid)))
		return_NULL;

	if (!(name = dm_build_dm_name(dm->mem, seg->lv->vg->name,
				   seg->lv->name, errid)))
		return_NULL;

	log_debug_activation("Getting device info for %s [%s]", name, dlid);
	if (!_info(dlid, 1, 0, &info, NULL)) {
		log_error("Failed to get info for %s [%s].", name, dlid);
		return 0;
	}

	if (!info.exists) {
		/* Create new node */
		if (!(node = dm_tree_add_new_dev(dtree, name, dlid, 0, 0, 0, 0, 0)))
			return_NULL;
		if (!dm_tree_node_add_error_target(node, size))
			return_NULL;
	} else {
		/* Already exists */
		if (!dm_tree_add_dev(dtree, info.major, info.minor)) {
			log_error("Failed to add device (%" PRIu32 ":%" PRIu32") to dtree",
				  info.major, info.minor);
			return_NULL;
		}
	}

	return dlid;
}

static int _add_error_area(struct dev_manager *dm, struct dm_tree_node *node,
			   struct lv_segment *seg, int s)
{
	char *dlid;
	uint64_t extent_size = seg->lv->vg->extent_size;

	if (!strcmp(dm->cmd->stripe_filler, "error")) {
		/*
		 * FIXME, the tree pointer is first field of dm_tree_node, but
		 * we don't have the struct definition available.
		 */
		struct dm_tree **tree = (struct dm_tree **) node;
		if (!(dlid = _add_error_device(dm, *tree, seg, s)))
			return_0;
		if (!dm_tree_node_add_target_area(node, NULL, dlid, extent_size * seg_le(seg, s)))
			return_0;
	} else
		if (!dm_tree_node_add_target_area(node, dm->cmd->stripe_filler, NULL, UINT64_C(0)))
			return_0;

	return 1;
}

int add_areas_line(struct dev_manager *dm, struct lv_segment *seg,
		   struct dm_tree_node *node, uint32_t start_area,
		   uint32_t areas)
{
	uint64_t extent_size = seg->lv->vg->extent_size;
	uint32_t s;
	char *dlid;
	struct stat info;
	const char *name;
	unsigned num_error_areas = 0;
	unsigned num_existing_areas = 0;

	/* FIXME Avoid repeating identical stat in dm_tree_node_add_target_area */
	for (s = start_area; s < areas; s++) {
		if ((seg_type(seg, s) == AREA_PV &&
		     (!seg_pvseg(seg, s) || !seg_pv(seg, s) || !seg_dev(seg, s) ||
		       !(name = dev_name(seg_dev(seg, s))) || !*name ||
		       stat(name, &info) < 0 || !S_ISBLK(info.st_mode))) ||
		    (seg_type(seg, s) == AREA_LV && !seg_lv(seg, s))) {
			if (!seg->lv->vg->cmd->partial_activation) {
				log_error("Aborting.  LV %s is now incomplete "
					  "and --partial was not specified.", seg->lv->name);
				return 0;
			}
			if (!_add_error_area(dm, node, seg, s))
				return_0;
			num_error_areas++;
		} else if (seg_type(seg, s) == AREA_PV) {
			if (!dm_tree_node_add_target_area(node, dev_name(seg_dev(seg, s)), NULL,
				    (seg_pv(seg, s)->pe_start + (extent_size * seg_pe(seg, s)))))
				return_0;
			num_existing_areas++;
		} else if (seg_is_raid(seg)) {
			/*
			 * RAID can handle unassigned areas.  It simple puts
			 * '- -' in for the metadata/data device pair.  This
			 * is a valid way to indicate to the RAID target that
			 * the device is missing.
			 *
			 * If an image is marked as VISIBLE_LV and !LVM_WRITE,
			 * it means the device has temporarily been extracted
			 * from the array.  It may come back at a future date,
			 * so the bitmap must track differences.  Again, '- -'
			 * is used in the CTR table.
			 */
			if ((seg_type(seg, s) == AREA_UNASSIGNED) ||
			    ((seg_lv(seg, s)->status & VISIBLE_LV) &&
			     !(seg_lv(seg, s)->status & LVM_WRITE))) {
				/* One each for metadata area and data area */
				if (!dm_tree_node_add_null_area(node, 0) ||
				    !dm_tree_node_add_null_area(node, 0))
					return_0;
				continue;
			}
			if (!(dlid = build_dm_uuid(dm->mem, seg_metalv(seg, s)->lvid.s, NULL)))
				return_0;
			if (!dm_tree_node_add_target_area(node, NULL, dlid, extent_size * seg_metale(seg, s)))
				return_0;

			if (!(dlid = build_dm_uuid(dm->mem, seg_lv(seg, s)->lvid.s, NULL)))
				return_0;
			if (!dm_tree_node_add_target_area(node, NULL, dlid, extent_size * seg_le(seg, s)))
				return_0;
		} else if (seg_type(seg, s) == AREA_LV) {

			if (!(dlid = build_dm_uuid(dm->mem, seg_lv(seg, s)->lvid.s, NULL)))
				return_0;
			if (!dm_tree_node_add_target_area(node, NULL, dlid, extent_size * seg_le(seg, s)))
				return_0;
		} else {
			log_error(INTERNAL_ERROR "Unassigned area found in LV %s.",
				  seg->lv->name);
			return 0;
		}
	}

        if (num_error_areas) {
		/* Thins currently do not support partial activation */
		if (lv_is_thin_type(seg->lv)) {
			log_error("Cannot activate %s%s: pool incomplete.",
				  seg->lv->vg->name, seg->lv->name);
			return 0;
		}
	}

	return 1;
}

static int _add_layer_target_to_dtree(struct dev_manager *dm,
				      struct dm_tree_node *dnode,
				      struct logical_volume *lv)
{
	const char *layer_dlid;

	if (!(layer_dlid = build_dm_uuid(dm->mem, lv->lvid.s, lv_layer(lv))))
		return_0;

	/* Add linear mapping over layered LV */
	if (!add_linear_area_to_dtree(dnode, lv->size, lv->vg->extent_size,
				      lv->vg->cmd->use_linear_target,
				      lv->vg->name, lv->name) ||
	    !dm_tree_node_add_target_area(dnode, NULL, layer_dlid, 0))
		return_0;

	return 1;
}

static int _add_origin_target_to_dtree(struct dev_manager *dm,
					 struct dm_tree_node *dnode,
					 struct logical_volume *lv)
{
	const char *real_dlid;

	if (!(real_dlid = build_dm_uuid(dm->mem, lv->lvid.s, "real")))
		return_0;

	if (!dm_tree_node_add_snapshot_origin_target(dnode, lv->size, real_dlid))
		return_0;

	return 1;
}

static int _add_snapshot_merge_target_to_dtree(struct dev_manager *dm,
					       struct dm_tree_node *dnode,
					       struct logical_volume *lv)
{
	const char *origin_dlid, *cow_dlid, *merge_dlid;
	struct lv_segment *merging_snap_seg;

	if (!(merging_snap_seg = find_merging_snapshot(lv))) {
		log_error(INTERNAL_ERROR "LV %s is not merging snapshot.", lv->name);
		return 0;
	}

	if (!(origin_dlid = build_dm_uuid(dm->mem, lv->lvid.s, "real")))
		return_0;

	if (!(cow_dlid = build_dm_uuid(dm->mem, merging_snap_seg->cow->lvid.s, "cow")))
		return_0;

	if (!(merge_dlid = build_dm_uuid(dm->mem, merging_snap_seg->cow->lvid.s, NULL)))
		return_0;

	if (!dm_tree_node_add_snapshot_merge_target(dnode, lv->size, origin_dlid,
						    cow_dlid, merge_dlid,
						    merging_snap_seg->chunk_size))
		return_0;

	return 1;
}

static int _add_snapshot_target_to_dtree(struct dev_manager *dm,
					 struct dm_tree_node *dnode,
					 struct logical_volume *lv,
					 struct lv_activate_opts *laopts)
{
	const char *origin_dlid;
	const char *cow_dlid;
	struct lv_segment *snap_seg;
	uint64_t size;

	if (!(snap_seg = find_snapshot(lv))) {
		log_error("Couldn't find snapshot for '%s'.", lv->name);
		return 0;
	}

	if (!(origin_dlid = build_dm_uuid(dm->mem, snap_seg->origin->lvid.s, "real")))
		return_0;

	if (!(cow_dlid = build_dm_uuid(dm->mem, snap_seg->cow->lvid.s, "cow")))
		return_0;

	size = (uint64_t) snap_seg->len * snap_seg->origin->vg->extent_size;

	if (!laopts->no_merging && lv_is_merging_cow(lv)) {
		/* cow is to be merged so load the error target */
		if (!dm_tree_node_add_error_target(dnode, size))
			return_0;
	}
	else if (!dm_tree_node_add_snapshot_target(dnode, size, origin_dlid,
						   cow_dlid, 1, snap_seg->chunk_size))
		return_0;

	return 1;
}

static int _add_target_to_dtree(struct dev_manager *dm,
				struct dm_tree_node *dnode,
				struct lv_segment *seg,
				struct lv_activate_opts *laopts)
{
	uint64_t extent_size = seg->lv->vg->extent_size;

	if (!seg->segtype->ops->add_target_line) {
		log_error(INTERNAL_ERROR "_emit_target cannot handle "
			  "segment type %s", seg->segtype->name);
		return 0;
	}

	return seg->segtype->ops->add_target_line(dm, dm->mem, dm->cmd,
						  &dm->target_state, seg,
						  laopts, dnode,
						  extent_size * seg->len,
						  &dm-> pvmove_mirror_count);
}

static int _add_new_lv_to_dtree(struct dev_manager *dm, struct dm_tree *dtree,
				struct logical_volume *lv,
				struct lv_activate_opts *laopts,
				const char *layer);

/* Add all replicators' LVs */
static int _add_replicator_dev_target_to_dtree(struct dev_manager *dm,
					       struct dm_tree *dtree,
					       struct lv_segment *seg,
					       struct lv_activate_opts *laopts)
{
	struct replicator_device *rdev;
	struct replicator_site *rsite;

	/* For inactive replicator add linear mapping */
	if (!lv_is_active_replicator_dev(seg->lv)) {
		if (!_add_new_lv_to_dtree(dm, dtree, seg->lv->rdevice->lv, laopts, NULL))
			return_0;
		return 1;
	}

	/* Add rlog and replicator nodes */
	if (!seg->replicator ||
	    !first_seg(seg->replicator)->rlog_lv ||
	    !_add_new_lv_to_dtree(dm, dtree,
				  first_seg(seg->replicator)->rlog_lv,
				  laopts, NULL) ||
	    !_add_new_lv_to_dtree(dm, dtree, seg->replicator, laopts, NULL))
	    return_0;

	/* Activation of one replicator_dev node activates all other nodes */
	dm_list_iterate_items(rsite, &seg->replicator->rsites) {
		dm_list_iterate_items(rdev, &rsite->rdevices) {
			if (rdev->lv &&
			    !_add_new_lv_to_dtree(dm, dtree, rdev->lv,
						  laopts, NULL))
				return_0;

			if (rdev->slog &&
			    !_add_new_lv_to_dtree(dm, dtree, rdev->slog,
						  laopts, NULL))
				return_0;
		}
	}
	/* Add remaining replicator-dev nodes in the second loop
	 * to avoid multiple retries for inserting all elements */
	dm_list_iterate_items(rsite, &seg->replicator->rsites) {
		if (rsite->state != REPLICATOR_STATE_ACTIVE)
			continue;
		dm_list_iterate_items(rdev, &rsite->rdevices) {
			if (rdev->replicator_dev->lv == seg->lv)
				continue;
			if (!rdev->replicator_dev->lv ||
			    !_add_new_lv_to_dtree(dm, dtree,
						  rdev->replicator_dev->lv,
						  laopts, NULL))
				return_0;
		}
	}

	return 1;
}

static int _add_new_external_lv_to_dtree(struct dev_manager *dm,
					 struct dm_tree *dtree,
					 struct logical_volume *external_lv,
					 struct lv_activate_opts *laopts)
{
	struct seg_list *sl;

	/* Do not want to recursively add externals again */
	if (dm->skip_external_lv)
		return 1;

	/*
	 * Any LV can have only 1 external origin, so we will
	 * process all LVs related to this LV, and we want to
	 * skip repeated invocation of external lv processing
	 */
	dm->skip_external_lv = 1;

	log_debug_activation("Adding external origin lv %s and all active users.",
			     external_lv->name);

	if (!_add_new_lv_to_dtree(dm, dtree, external_lv, laopts,
				  lv_layer(external_lv)))
		return_0;

	/*
	 * Add all ACTIVE LVs using this external origin LV. This is
	 * needed because of conversion of thin which could have been
	 * also an old-snapshot to external origin.
	 */
	//if (lv_is_origin(external_lv))
	dm_list_iterate_items(sl, &external_lv->segs_using_this_lv)
		if ((sl->seg->external_lv == external_lv) &&
		    /* Add only active layered devices (also avoids loop) */
		    _cached_info(dm->mem, dtree, sl->seg->lv,
				 lv_layer(sl->seg->lv)) &&
		    !_add_new_lv_to_dtree(dm, dtree, sl->seg->lv,
					  laopts, lv_layer(sl->seg->lv)))
			return_0;

	log_debug_activation("Finished adding  external origin lv %s and all active users.",
			     external_lv->name);
	dm->skip_external_lv = 0;

	return 1;
}

static int _add_segment_to_dtree(struct dev_manager *dm,
				 struct dm_tree *dtree,
				 struct dm_tree_node *dnode,
				 struct lv_segment *seg,
				 struct lv_activate_opts *laopts,
				 const char *layer)
{
	uint32_t s;
	struct lv_segment *seg_present;
	const char *target_name;

	/* Ensure required device-mapper targets are loaded */
	seg_present = find_snapshot(seg->lv) ? : seg;
	target_name = (seg_present->segtype->ops->target_name ?
		       seg_present->segtype->ops->target_name(seg_present, laopts) :
		       seg_present->segtype->name);

	log_debug_activation("Checking kernel supports %s segment type for %s%s%s",
			     target_name, seg->lv->name,
			     layer ? "-" : "", layer ? : "");

	if (seg_present->segtype->ops->target_present &&
	    !seg_present->segtype->ops->target_present(seg_present->lv->vg->cmd,
						       seg_present, NULL)) {
		log_error("Can't process LV %s: %s target support missing "
			  "from kernel?", seg->lv->name, target_name);
		return 0;
	}

	/* Add external origin layer */
	if (seg->external_lv &&
	    !_add_new_external_lv_to_dtree(dm, dtree, seg->external_lv, laopts))
		return_0;
	/* Add mirror log */
	if (seg->log_lv &&
	    !_add_new_lv_to_dtree(dm, dtree, seg->log_lv, laopts, NULL))
		return_0;
	/* Add thin pool metadata */
	if (seg->metadata_lv &&
	    !_add_new_lv_to_dtree(dm, dtree, seg->metadata_lv, laopts, NULL))
		return_0;
	/* Add thin pool layer */
	if (seg->pool_lv &&
	    !_add_new_lv_to_dtree(dm, dtree, seg->pool_lv, laopts,
				  lv_layer(seg->pool_lv)))
		return_0;

	if (seg_is_replicator_dev(seg)) {
		if (!_add_replicator_dev_target_to_dtree(dm, dtree, seg, laopts))
			return_0;
	}

	/* Add any LVs used by this segment */
	for (s = 0; s < seg->area_count; ++s) {
		if ((seg_type(seg, s) == AREA_LV) &&
		    (!_add_new_lv_to_dtree(dm, dtree, seg_lv(seg, s),
					   laopts, NULL)))
			return_0;
		if (seg_is_raid(seg) &&
		    !_add_new_lv_to_dtree(dm, dtree, seg_metalv(seg, s),
					  laopts, NULL))
			return_0;
	}

	if (!_add_target_to_dtree(dm, dnode, seg, laopts))
		return_0;

	return 1;
}

static int _set_udev_flags_for_children(struct dev_manager *dm,
					struct volume_group *vg,
					struct dm_tree_node *dnode)
{
	char *p;
	const char *uuid;
	void *handle = NULL;
	struct dm_tree_node *child;
	const struct dm_info *info;
	struct lv_list *lvl;

	while ((child = dm_tree_next_child(&handle, dnode, 0))) {
		/* Ignore root node */
		if (!(info  = dm_tree_node_get_info(child)) || !info->exists)
			continue;

		if (!(uuid = dm_tree_node_get_uuid(child))) {
			log_error(INTERNAL_ERROR
				  "Failed to get uuid for %" PRIu32 ":%" PRIu32,
				  info->major, info->minor);
			continue;
		}

		/* Ignore non-LVM devices */
		if (!(p = strstr(uuid, UUID_PREFIX)))
			continue;
		p += strlen(UUID_PREFIX);

		/* Ignore LVs that belong to different VGs (due to stacking) */
		if (strncmp(p, (char *)vg->id.uuid, ID_LEN))
			continue;

		/* Ignore LVM devices with 'layer' suffixes */
		if (strrchr(p, '-'))
			continue;

		if (!(lvl = find_lv_in_vg_by_lvid(vg, (const union lvid *)p))) {
			log_error(INTERNAL_ERROR
				  "%s (%" PRIu32 ":%" PRIu32 ") not found in VG",
				  dm_tree_node_get_name(child),
				  info->major, info->minor);
			return 0;
		}

		dm_tree_node_set_udev_flags(child,
					    _get_udev_flags(dm, lvl->lv, NULL));
	}

	return 1;
}

static int _add_new_lv_to_dtree(struct dev_manager *dm, struct dm_tree *dtree,
				struct logical_volume *lv, struct lv_activate_opts *laopts,
				const char *layer)
{
	struct lv_segment *seg;
	struct lv_layer *lvlayer;
	struct seg_list *sl;
	struct dm_list *snh;
	struct dm_tree_node *dnode;
	const struct dm_info *dinfo;
	char *name, *dlid;
	uint32_t max_stripe_size = UINT32_C(0);
	uint32_t read_ahead = lv->read_ahead;
	uint32_t read_ahead_flags = UINT32_C(0);

	/* FIXME Seek a simpler way to lay out the snapshot-merge tree. */

	if (lv_is_origin(lv) && lv_is_merging_origin(lv) && !layer) {
		/*
		 * Clear merge attributes if merge isn't currently possible:
		 * either origin or merging snapshot are open
		 * - but use "snapshot-merge" if it is already in use
		 * - open_count is always retrieved (as of dm-ioctl 4.7.0)
		 *   so just use the tree's existing nodes' info
		 */
		/* An activating merging origin won't have a node in the tree yet */
		if (((dinfo = _cached_info(dm->mem, dtree, lv, NULL)) &&
		     dinfo->open_count) ||
		    ((dinfo = _cached_info(dm->mem, dtree,
					   find_merging_snapshot(lv)->cow, NULL)) &&
		     dinfo->open_count)) {
			/* FIXME Is there anything simpler to check for instead? */
			if (!lv_has_target_type(dm->mem, lv, NULL, "snapshot-merge"))
				laopts->no_merging = 1;
		}
	}

	if (!(name = dm_build_dm_name(dm->mem, lv->vg->name, lv->name, layer)))
		return_0;

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, layer)))
		return_0;

	/* We've already processed this node if it already has a context ptr */
	if ((dnode = dm_tree_find_node_by_uuid(dtree, dlid)) &&
	    dm_tree_node_get_context(dnode))
		return 1;

	if (!(lvlayer = dm_pool_alloc(dm->mem, sizeof(*lvlayer)))) {
		log_error("_add_new_lv_to_dtree: pool alloc failed for %s %s.",
			  lv->name, layer);
		return 0;
	}

	lvlayer->lv = lv;

	/*
	 * Add LV to dtree.
	 * If we're working with precommitted metadata, clear any
	 * existing inactive table left behind.
	 * Major/minor settings only apply to the visible layer.
	 */
	/* FIXME Move the clear from here until later, so we can leave
	 * identical inactive tables untouched. (For pvmove.)
	 */
	if (!(dnode = dm_tree_add_new_dev_with_udev_flags(dtree, name, dlid,
					     layer ? UINT32_C(0) : (uint32_t) lv->major,
					     layer ? UINT32_C(0) : (uint32_t) lv->minor,
					     read_only_lv(lv, laopts),
					     ((lv->vg->status & PRECOMMITTED) | laopts->revert) ? 1 : 0,
					     lvlayer,
					     _get_udev_flags(dm, lv, layer))))
		return_0;

	/* Store existing name so we can do rename later */
	lvlayer->old_name = dm_tree_node_get_name(dnode);

	/* Create table */
	dm->pvmove_mirror_count = 0u;

	/* If this is a snapshot origin, add real LV */
	/* If this is a snapshot origin + merging snapshot, add cow + real LV */
	/* Snapshot origin could be also external origin */
	if (lv_is_origin(lv) && !layer) {
		if (!_add_new_lv_to_dtree(dm, dtree, lv, laopts, "real"))
			return_0;
		if (!laopts->no_merging && lv_is_merging_origin(lv)) {
			if (!_add_new_lv_to_dtree(dm, dtree,
			     find_merging_snapshot(lv)->cow, laopts, "cow"))
				return_0;
			/*
			 * Must also add "real" LV for use when
			 * snapshot-merge target is added
			 */
			if (!_add_snapshot_merge_target_to_dtree(dm, dnode, lv))
				return_0;
		} else if (!_add_origin_target_to_dtree(dm, dnode, lv))
			return_0;

		/* Add any snapshots of this LV */
		dm_list_iterate(snh, &lv->snapshot_segs)
			if (!_add_new_lv_to_dtree(dm, dtree,
						  dm_list_struct_base(snh, struct lv_segment,
								      origin_list)->cow,
						  laopts, NULL))
				return_0;
	} else if (lv_is_cow(lv) && !layer) {
		if (!_add_new_lv_to_dtree(dm, dtree, lv, laopts, "cow"))
			return_0;
		if (!_add_snapshot_target_to_dtree(dm, dnode, lv, laopts))
			return_0;
	} else if ((lv_is_external_origin(lv) || lv_is_thin_pool(lv)) && !layer) {
		/* External origin or Thin pool is using layer */
		if (!_add_new_lv_to_dtree(dm, dtree, lv, laopts, lv_layer(lv)))
			return_0;
		if (!_add_layer_target_to_dtree(dm, dnode, lv))
			return_0;
	} else {
		/* Add 'real' segments for LVs */
		dm_list_iterate_items(seg, &lv->segments) {
			if (!_add_segment_to_dtree(dm, dtree, dnode, seg, laopts, layer))
				return_0;
			if (max_stripe_size < seg->stripe_size * seg->area_count)
				max_stripe_size = seg->stripe_size * seg->area_count;
		}
	}

	/* Setup thin pool callback */
	if (lv_is_thin_pool(lv) && layer &&
	    !_thin_pool_register_callback(dm, dnode, lv))
		return_0;

	if (read_ahead == DM_READ_AHEAD_AUTO) {
		/* we need RA at least twice a whole stripe - see the comment in md/raid0.c */
		read_ahead = max_stripe_size * 2;
		/* FIXME: layered device read-ahead */
		if (!read_ahead)
			lv_calculate_readahead(lv, &read_ahead);
		read_ahead_flags = DM_READ_AHEAD_MINIMUM_FLAG;
	}

	dm_tree_node_set_read_ahead(dnode, read_ahead, read_ahead_flags);

	/* Add any LVs referencing a PVMOVE LV unless told not to */
	if (dm->track_pvmove_deps && (lv->status & PVMOVE))
		dm_list_iterate_items(sl, &lv->segs_using_this_lv)
			if (!_add_new_lv_to_dtree(dm, dtree, sl->seg->lv, laopts, NULL))
				return_0;

	if (!_set_udev_flags_for_children(dm, lv->vg, dnode))
		return_0;

	return 1;
}

/* FIXME: symlinks should be created/destroyed at the same time
 * as the kernel devices but we can't do that from within libdevmapper
 * at present so we must walk the tree twice instead. */

/*
 * Create LV symlinks for children of supplied root node.
 */
static int _create_lv_symlinks(struct dev_manager *dm, struct dm_tree_node *root)
{
	void *handle = NULL;
	struct dm_tree_node *child;
	struct lv_layer *lvlayer;
	char *old_vgname, *old_lvname, *old_layer;
	char *new_vgname, *new_lvname, *new_layer;
	const char *name;
	int r = 1;

	/* Nothing to do if udev fallback is disabled. */
	if (!_check_udev_fallback(dm->cmd)) {
		fs_set_create();
		return 1;
	}

	while ((child = dm_tree_next_child(&handle, root, 0))) {
		if (!(lvlayer = dm_tree_node_get_context(child)))
			continue;

		/* Detect rename */
		name = dm_tree_node_get_name(child);

		if (name && lvlayer->old_name && *lvlayer->old_name && strcmp(name, lvlayer->old_name)) {
			if (!dm_split_lvm_name(dm->mem, lvlayer->old_name, &old_vgname, &old_lvname, &old_layer)) {
				log_error("_create_lv_symlinks: Couldn't split up old device name %s", lvlayer->old_name);
				return 0;
			}
			if (!dm_split_lvm_name(dm->mem, name, &new_vgname, &new_lvname, &new_layer)) {
				log_error("_create_lv_symlinks: Couldn't split up new device name %s", name);
				return 0;
			}
			if (!fs_rename_lv(lvlayer->lv, name, old_vgname, old_lvname))
				r = 0;
			continue;
		}
		if (lv_is_visible(lvlayer->lv)) {
			if (!_dev_manager_lv_mknodes(lvlayer->lv))
				r = 0;
			continue;
		}
		if (!_dev_manager_lv_rmnodes(lvlayer->lv))
			r = 0;
	}

	return r;
}

/*
 * Remove LV symlinks for children of supplied root node.
 */
static int _remove_lv_symlinks(struct dev_manager *dm, struct dm_tree_node *root)
{
	void *handle = NULL;
	struct dm_tree_node *child;
	char *vgname, *lvname, *layer;
	int r = 1;

	/* Nothing to do if udev fallback is disabled. */
	if (!_check_udev_fallback(dm->cmd))
		return 1;

	while ((child = dm_tree_next_child(&handle, root, 0))) {
		if (!dm_split_lvm_name(dm->mem, dm_tree_node_get_name(child), &vgname, &lvname, &layer)) {
			r = 0;
			continue;
		}

		if (!*vgname)
			continue;

		/* only top level layer has symlinks */
		if (*layer)
			continue;

		fs_del_lv_byname(dm->cmd->dev_dir, vgname, lvname,
				 dm->cmd->current_settings.udev_rules);
	}

	return r;
}

static int _clean_tree(struct dev_manager *dm, struct dm_tree_node *root, char *non_toplevel_tree_dlid)
{
	void *handle = NULL;
	struct dm_tree_node *child;
	char *vgname, *lvname, *layer;
	const char *name, *uuid;

	while ((child = dm_tree_next_child(&handle, root, 0))) {
		if (!(name = dm_tree_node_get_name(child)))
			continue;

		if (!(uuid = dm_tree_node_get_uuid(child)))
			continue;

		if (!dm_split_lvm_name(dm->mem, name, &vgname, &lvname, &layer)) {
			log_error("_clean_tree: Couldn't split up device name %s.", name);
			return 0;
		}

		/* Not meant to be top level? */
		if (!*layer)
			continue;

		/* If operation was performed on a partial tree, don't remove it */
		if (non_toplevel_tree_dlid && !strcmp(non_toplevel_tree_dlid, uuid))
			continue;

		if (!dm_tree_deactivate_children(root, uuid, strlen(uuid)))
			return_0;
	}

	return 1;
}

static int _tree_action(struct dev_manager *dm, struct logical_volume *lv,
			struct lv_activate_opts *laopts, action_t action)
{
	const size_t DLID_SIZE = ID_LEN + sizeof(UUID_PREFIX) - 1;
	struct dm_tree *dtree;
	struct dm_tree_node *root;
	char *dlid;
	int r = 0;

	/* Some targets may build bigger tree for activation */
	dm->activation = ((action == PRELOAD) || (action == ACTIVATE));
	if (!(dtree = _create_partial_dtree(dm, lv, laopts->origin_only)))
		return_0;

	if (!(root = dm_tree_find_node(dtree, 0, 0))) {
		log_error("Lost dependency tree root node");
		goto out_no_root;
	}

	/* Restore fs cookie */
	dm_tree_set_cookie(root, fs_get_cookie());

	if (!(dlid = build_dm_uuid(dm->mem, lv->lvid.s, laopts->origin_only ? lv_layer(lv) : NULL)))
		goto_out;

	/* Only process nodes with uuid of "LVM-" plus VG id. */
	switch(action) {
	case CLEAN:
		/* Deactivate any unused non-toplevel nodes */
		if (!_clean_tree(dm, root, laopts->origin_only ? dlid : NULL))
			goto_out;
		break;
	case DEACTIVATE:
		if (retry_deactivation())
			dm_tree_retry_remove(root);
		/* Deactivate LV and all devices it references that nothing else has open. */
		if (!dm_tree_deactivate_children(root, dlid, DLID_SIZE))
			goto_out;
		if (!_remove_lv_symlinks(dm, root))
			log_warn("Failed to remove all device symlinks associated with %s.", lv->name);
		break;
	case SUSPEND:
		dm_tree_skip_lockfs(root);
		if (!dm->flush_required && !seg_is_raid(first_seg(lv)) &&
		    (lv->status & MIRRORED) && !(lv->status & PVMOVE))
			dm_tree_use_no_flush_suspend(root);
		/* Fall through */
	case SUSPEND_WITH_LOCKFS:
		if (!dm_tree_suspend_children(root, dlid, DLID_SIZE))
			goto_out;
		break;
	case PRELOAD:
	case ACTIVATE:
		/* Add all required new devices to tree */
		if (!_add_new_lv_to_dtree(dm, dtree, lv, laopts, (lv_is_origin(lv) && laopts->origin_only) ? "real" : NULL))
			goto_out;

		/* Preload any devices required before any suspensions */
		if (!dm_tree_preload_children(root, dlid, DLID_SIZE))
			goto_out;

		if (dm_tree_node_size_changed(root))
			dm->flush_required = 1;

		if (action == ACTIVATE) {
			if (!dm_tree_activate_children(root, dlid, DLID_SIZE))
				goto_out;
			if (!_create_lv_symlinks(dm, root))
				log_warn("Failed to create symlinks for %s.", lv->name);
		}

		break;
	default:
		log_error("_tree_action: Action %u not supported.", action);
		goto out;
	}

	r = 1;

out:
	/* Save fs cookie for udev settle, do not wait here */
	fs_set_cookie(dm_tree_get_cookie(root));
out_no_root:
	dm_tree_free(dtree);

	return r;
}

/* origin_only may only be set if we are resuming (not activating) an origin LV */
int dev_manager_activate(struct dev_manager *dm, struct logical_volume *lv,
			 struct lv_activate_opts *laopts)
{
	laopts->send_messages = 1;
	if (!_tree_action(dm, lv, laopts, ACTIVATE))
		return_0;

	if (!_tree_action(dm, lv, laopts, CLEAN))
		return_0;

	return 1;
}

/* origin_only may only be set if we are resuming (not activating) an origin LV */
int dev_manager_preload(struct dev_manager *dm, struct logical_volume *lv,
			struct lv_activate_opts *laopts, int *flush_required)
{
	if (!_tree_action(dm, lv, laopts, PRELOAD))
		return_0;

	*flush_required = dm->flush_required;

	return 1;
}

int dev_manager_deactivate(struct dev_manager *dm, struct logical_volume *lv)
{
	struct lv_activate_opts laopts = { 0 };

	if (!_tree_action(dm, lv, &laopts, DEACTIVATE))
		return_0;

	return 1;
}

int dev_manager_suspend(struct dev_manager *dm, struct logical_volume *lv,
			struct lv_activate_opts *laopts, int lockfs, int flush_required)
{
	dm->flush_required = flush_required;

	if (!_tree_action(dm, lv, laopts, lockfs ? SUSPEND_WITH_LOCKFS : SUSPEND))
		return_0;

	return 1;
}

/*
 * Does device use VG somewhere in its construction?
 * Returns 1 if uncertain.
 */
int dev_manager_device_uses_vg(struct device *dev,
			       struct volume_group *vg)
{
	struct dm_tree *dtree;
	struct dm_tree_node *root;
	char dlid[sizeof(UUID_PREFIX) + sizeof(struct id) - 1] __attribute__((aligned(8)));
	int r = 1;

	if (!(dtree = dm_tree_create())) {
		log_error("partial dtree creation failed");
		return r;
	}

	if (!dm_tree_add_dev(dtree, (uint32_t) MAJOR(dev->dev), (uint32_t) MINOR(dev->dev))) {
		log_error("Failed to add device %s (%" PRIu32 ":%" PRIu32") to dtree",
			  dev_name(dev), (uint32_t) MAJOR(dev->dev), (uint32_t) MINOR(dev->dev));
		goto out;
	}

	memcpy(dlid, UUID_PREFIX, sizeof(UUID_PREFIX) - 1);
	memcpy(dlid + sizeof(UUID_PREFIX) - 1, &vg->id.uuid[0], sizeof(vg->id));

	if (!(root = dm_tree_find_node(dtree, 0, 0))) {
		log_error("Lost dependency tree root node");
		goto out;
	}

	if (dm_tree_children_use_uuid(root, dlid, sizeof(UUID_PREFIX) + sizeof(vg->id) - 1))
		goto_out;

	r = 0;

out:
	dm_tree_free(dtree);
	return r;
}
