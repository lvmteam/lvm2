/*
 * Copyright (C) 2005-2007 Red Hat, Inc. All rights reserved.
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

#include "tools.h"
#include "polldaemon.h"
#include "lv_alloc.h"
#include "metadata.h"

struct lvconvert_params {
	int snapshot;
	int merge;
	int merge_mirror;
	int zero;

	const char *origin;
	const char *lv_name;
	const char *lv_split_name;
	const char *lv_name_full;
	const char *vg_name;
	int wait_completion;
	int need_polling;

	uint32_t chunk_size;
	uint32_t region_size;

	uint32_t mirrors;
	sign_t mirrors_sign;
	uint32_t keep_mimages;
	uint32_t stripes;
	uint32_t stripe_size;

	const struct segment_type *segtype;

	alloc_policy_t alloc;

	int pv_count;
	char **pvs;
	struct dm_list *pvh;

	int replace_pv_count;
	char **replace_pvs;
	struct dm_list *replace_pvh;

	struct logical_volume *lv_to_poll;
};

static int _lvconvert_name_params(struct lvconvert_params *lp,
				  struct cmd_context *cmd,
				  int *pargc, char ***pargv)
{
	char *ptr;
	const char *vg_name = NULL;

	if (lp->merge)
		return 1;

	if (lp->snapshot) {
		if (!*pargc) {
			log_error("Please specify a logical volume to act as "
				  "the snapshot origin.");
			return 0;
		}

		lp->origin = *pargv[0];
		(*pargv)++, (*pargc)--;
		if (!(lp->vg_name = extract_vgname(cmd, lp->origin))) {
			log_error("The origin name should include the "
				  "volume group.");
			return 0;
		}

		/* Strip the volume group from the origin */
		if ((ptr = strrchr(lp->origin, (int) '/')))
			lp->origin = ptr + 1;
	}

	if (!*pargc) {
		log_error("Please provide logical volume path");
		return 0;
	}

	lp->lv_name = lp->lv_name_full = (*pargv)[0];
	(*pargv)++, (*pargc)--;

	if (strchr(lp->lv_name_full, '/') &&
	    (vg_name = extract_vgname(cmd, lp->lv_name_full)) &&
	    lp->vg_name && strcmp(vg_name, lp->vg_name)) {
		log_error("Please use a single volume group name "
			  "(\"%s\" or \"%s\")", vg_name, lp->vg_name);
		return 0;
	}

	if (!lp->vg_name)
		lp->vg_name = vg_name;

	if (!validate_name(lp->vg_name)) {
		log_error("Please provide a valid volume group name");
		return 0;
	}

	if ((ptr = strrchr(lp->lv_name_full, '/')))
		lp->lv_name = ptr + 1;

	if (!lp->merge_mirror && !apply_lvname_restrictions(lp->lv_name))
		return_0;

	if (*pargc && lp->snapshot) {
		log_error("Too many arguments provided for snapshots");
		return 0;
	}

	return 1;
}

static int _read_params(struct lvconvert_params *lp, struct cmd_context *cmd,
			int argc, char **argv)
{
	int i;
	const char *tmp_str;
	struct arg_value_group_list *group;
	int region_size;
	int pagesize = lvm_getpagesize();

	memset(lp, 0, sizeof(*lp));

	if ((arg_count(cmd, snapshot_ARG) || arg_count(cmd, merge_ARG)) &&
	    (arg_count(cmd, mirrorlog_ARG) || arg_count(cmd, mirrors_ARG) ||
	     arg_count(cmd, repair_ARG))) {
		log_error("--snapshot or --merge argument cannot be mixed "
			  "with --mirrors, --repair or --mirrorlog");
		return 0;
	}

	if (!arg_count(cmd, background_ARG))
		lp->wait_completion = 1;

	if (arg_count(cmd, snapshot_ARG))
		lp->snapshot = 1;

	if (arg_count(cmd, snapshot_ARG) && arg_count(cmd, merge_ARG)) {
		log_error("--snapshot and --merge are mutually exclusive");
		return 0;
	}

	if (arg_count(cmd, splitmirrors_ARG) && arg_count(cmd, mirrors_ARG)) {
		log_error("--mirrors and --splitmirrors are "
			  "mutually exclusive");
		return 0;
	}

	/*
	 * The '--splitmirrors n' argument is equivalent to '--mirrors -n'
	 * (note the minus sign), except that it signifies the additional
	 * intent to keep the mimage that is detached, rather than
	 * discarding it.
	 */
	if (arg_count(cmd, splitmirrors_ARG)) {
		if (!arg_count(cmd, name_ARG) &&
		    !arg_count(cmd, trackchanges_ARG)) {
			log_error("Please name the new logical volume using '--name'");
			return 0;
		}

		lp->lv_split_name = arg_value(cmd, name_ARG);
		if (lp->lv_split_name &&
		    !apply_lvname_restrictions(lp->lv_split_name))
			return_0;

		lp->keep_mimages = 1;
		lp->mirrors = arg_uint_value(cmd, splitmirrors_ARG, 0);
		lp->mirrors_sign = SIGN_MINUS;
	} else if (arg_count(cmd, name_ARG)) {
		log_error("The 'name' argument is only valid"
			  " with --splitmirrors");
		return 0;
	}

	if (arg_count(cmd, merge_ARG)) {
		if ((argc == 1) && strstr(argv[0], "_rimage_"))
			lp->merge_mirror = 1;
		else
			lp->merge = 1;
	}

	if (arg_count(cmd, mirrors_ARG)) {
		/*
		 * --splitmirrors has been chosen as the mechanism for
		 * specifying the intent of detaching and keeping a mimage
		 * versus an additional qualifying argument being added here.
		 */
		lp->mirrors = arg_uint_value(cmd, mirrors_ARG, 0);
		lp->mirrors_sign = arg_sign_value(cmd, mirrors_ARG, 0);
	}

	lp->alloc = arg_uint_value(cmd, alloc_ARG, ALLOC_INHERIT);

	/* There are three types of lvconvert. */
	if (lp->merge) {	/* Snapshot merge */
		if (arg_count(cmd, regionsize_ARG) || arg_count(cmd, chunksize_ARG) ||
		    arg_count(cmd, zero_ARG) || arg_count(cmd, regionsize_ARG) ||
		    arg_count(cmd, stripes_long_ARG) || arg_count(cmd, stripesize_ARG)) {
			log_error("Only --background and --interval are valid "
				  "arguments for snapshot merge");
			return 0;
		}

		if (!(lp->segtype = get_segtype_from_string(cmd, "snapshot")))
			return_0;

	} else if (lp->snapshot) {	/* Snapshot creation from pre-existing cow */
		if (arg_count(cmd, regionsize_ARG)) {
			log_error("--regionsize is only available with mirrors");
			return 0;
		}

		if (arg_count(cmd, stripesize_ARG) || arg_count(cmd, stripes_long_ARG)) {
			log_error("--stripes and --stripesize are only available with striped mirrors");
			return 0;
		}

		if (arg_sign_value(cmd, chunksize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative chunk size is invalid");
			return 0;
		}
		lp->chunk_size = arg_uint_value(cmd, chunksize_ARG, 8);
		if (lp->chunk_size < 8 || lp->chunk_size > 1024 ||
		    (lp->chunk_size & (lp->chunk_size - 1))) {
			log_error("Chunk size must be a power of 2 in the "
				  "range 4K to 512K");
			return 0;
		}
		log_verbose("Setting chunksize to %d sectors.", lp->chunk_size);

		if (!(lp->segtype = get_segtype_from_string(cmd, "snapshot")))
			return_0;

		lp->zero = strcmp(arg_str_value(cmd, zero_ARG,
						(lp->segtype->flags &
						 SEG_CANNOT_BE_ZEROED) ?
						"n" : "y"), "n");

	} else if (arg_count(cmd, replace_ARG)) { /* RAID device replacement */
		lp->replace_pv_count = arg_count(cmd, replace_ARG);
		lp->replace_pvs = dm_pool_alloc(cmd->mem, sizeof(char *) * lp->replace_pv_count);
		if (!lp->replace_pvs)
			return_0;

		i = 0;
		dm_list_iterate_items(group, &cmd->arg_value_groups) {
			if (!grouped_arg_is_set(group->arg_values, replace_ARG))
				continue;
			if (!(tmp_str = grouped_arg_str_value(group->arg_values,
							      replace_ARG,
							      NULL))) {
				log_error("Failed to get '--replace' argument");
				return 0;
			}
			if (!(lp->replace_pvs[i++] = dm_pool_strdup(cmd->mem,
								    tmp_str)))
				return_0;
		}
	} else { /* Mirrors (and some RAID functions) */
		if (arg_count(cmd, chunksize_ARG)) {
			log_error("--chunksize is only available with "
				  "snapshots");
			return 0;
		}

		if (arg_count(cmd, zero_ARG)) {
			log_error("--zero is only available with snapshots");
			return 0;
		}

		/*
	 	 * --regionsize is only valid if converting an LV into a mirror.
	 	 * Checked when we know the state of the LV being converted.
	 	 */

		if (arg_count(cmd, regionsize_ARG)) {
			if (arg_sign_value(cmd, regionsize_ARG, 0) ==
				    SIGN_MINUS) {
				log_error("Negative regionsize is invalid");
				return 0;
			}
			lp->region_size = arg_uint_value(cmd, regionsize_ARG, 0);
		} else {
			region_size = 2 * find_config_tree_int(cmd,
						"activation/mirror_region_size",
						DEFAULT_MIRROR_REGION_SIZE);
			if (region_size < 0) {
				log_error("Negative regionsize in "
					  "configuration file is invalid");
				return 0;
			}
			lp->region_size = region_size;
		}

		if (lp->region_size % (pagesize >> SECTOR_SHIFT)) {
			log_error("Region size (%" PRIu32 ") must be "
				  "a multiple of machine memory "
				  "page size (%d)",
				  lp->region_size, pagesize >> SECTOR_SHIFT);
			return 0;
		}

		if (lp->region_size & (lp->region_size - 1)) {
			log_error("Region size (%" PRIu32
				  ") must be a power of 2", lp->region_size);
			return 0;
		}

		if (!lp->region_size) {
			log_error("Non-zero region size must be supplied.");
			return 0;
		}

		/* Default is never striped, regardless of existing LV configuration. */
		if (!get_stripe_params(cmd, &lp->stripes, &lp->stripe_size)) {
			stack;
			return 0;
		}

		lp->segtype = get_segtype_from_string(cmd, arg_str_value(cmd, type_ARG, "mirror"));
		if (!lp->segtype)
			return_0;
	}

	if (activation() && lp->segtype && lp->segtype->ops->target_present &&
	    !lp->segtype->ops->target_present(cmd, NULL, NULL)) {
		log_error("%s: Required device-mapper target(s) not "
			  "detected in your kernel", lp->segtype->name);
		return 0;
	}

	if (!_lvconvert_name_params(lp, cmd, &argc, &argv))
		return_0;

	lp->pv_count = argc;
	lp->pvs = argv;

	return 1;
}

static struct volume_group *_get_lvconvert_vg(struct cmd_context *cmd,
					      const char *name,
					      const char *uuid __attribute__((unused)))
{
	dev_close_all();

	if (name && !strchr(name, '/'))
		return vg_read_for_update(cmd, name, NULL, 0);

	/* 'name' is the full LV name; must extract_vgname() */
	return vg_read_for_update(cmd, extract_vgname(cmd, name),
				  NULL, 0);
}

static struct logical_volume *_get_lvconvert_lv(struct cmd_context *cmd __attribute__((unused)),
						struct volume_group *vg,
						const char *name,
						const char *uuid,
						uint64_t lv_type __attribute__((unused)))
{
	struct logical_volume *lv = find_lv(vg, name);

	if (!lv || (uuid && strcmp(uuid, (char *)&lv->lvid)))
		return NULL;

	return lv;
}

static int _finish_lvconvert_mirror(struct cmd_context *cmd,
				    struct volume_group *vg,
				    struct logical_volume *lv,
				    struct dm_list *lvs_changed __attribute__((unused)))
{
	int r = 0;

	if (!(lv->status & CONVERTING))
		return 1;

	if (!collapse_mirrored_lv(lv)) {
		log_error("Failed to remove temporary sync layer.");
		return 0;
	}

	lv->status &= ~CONVERTING;

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	if (!vg_write(vg))
		return_0;

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(vg);
		goto out;
	}

	if (!vg_commit(vg)) {
		resume_lv(cmd, lv);
		goto_out;
	}

	log_very_verbose("Updating \"%s\" in kernel", lv->name);

	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		goto out;
	}

	r = 1;
	log_print("Logical volume %s converted.", lv->name);
out:
	backup(vg);
	return r;
}

static int _finish_lvconvert_merge(struct cmd_context *cmd,
				   struct volume_group *vg,
				   struct logical_volume *lv,
				   struct dm_list *lvs_changed __attribute__((unused)))
{
	struct lv_segment *snap_seg = find_merging_cow(lv);
	if (!snap_seg) {
		log_error("Logical volume %s has no merging snapshot.", lv->name);
		return 0;
	}

	log_print("Merge of snapshot into logical volume %s has finished.", lv->name);
	if (!lv_remove_single(cmd, snap_seg->cow, DONT_PROMPT)) {
		log_error("Could not remove snapshot %s merged into %s.",
			  snap_seg->cow->name, lv->name);
		return 0;
	}

	return 1;
}

static progress_t _poll_merge_progress(struct cmd_context *cmd,
				       struct logical_volume *lv,
				       const char *name __attribute__((unused)),
				       struct daemon_parms *parms)
{
	percent_t percent = PERCENT_0;

	if (!lv_snapshot_percent(lv, &percent)) {
		log_error("%s: Failed query for merging percentage. Aborting merge.", lv->name);
		return PROGRESS_CHECK_FAILED;
	} else if (percent == PERCENT_INVALID) {
		log_error("%s: Merging snapshot invalidated. Aborting merge.", lv->name);
		return PROGRESS_CHECK_FAILED;
	} else if (percent == PERCENT_MERGE_FAILED) {
		log_error("%s: Merge failed. Retry merge or inspect manually.", lv->name);
		return PROGRESS_CHECK_FAILED;
	}

	if (parms->progress_display)
		log_print("%s: %s: %.1f%%", lv->name, parms->progress_title,
			  percent_to_float(percent));
	else
		log_verbose("%s: %s: %.1f%%", lv->name, parms->progress_title,
			    percent_to_float(percent));

	if (percent == PERCENT_0)
		return PROGRESS_FINISHED_ALL;

	return PROGRESS_UNFINISHED;
}

static struct poll_functions _lvconvert_mirror_fns = {
	.get_copy_vg = _get_lvconvert_vg,
	.get_copy_lv = _get_lvconvert_lv,
	.poll_progress = poll_mirror_progress,
	.finish_copy = _finish_lvconvert_mirror,
};

static struct poll_functions _lvconvert_merge_fns = {
	.get_copy_vg = _get_lvconvert_vg,
	.get_copy_lv = _get_lvconvert_lv,
	.poll_progress = _poll_merge_progress,
	.finish_copy = _finish_lvconvert_merge,
};

int lvconvert_poll(struct cmd_context *cmd, struct logical_volume *lv,
		   unsigned background)
{
	/*
	 * FIXME allocate an "object key" structure with split
	 * out members (vg_name, lv_name, uuid, etc) and pass that
	 * around the lvconvert and polldaemon code
	 * - will avoid needless work, e.g. extract_vgname()
	 * - unfortunately there are enough overloaded "name" dragons in
	 *   the polldaemon, lvconvert, pvmove code that a comprehensive
	 *   audit/rework is needed
	 */
	int len = strlen(lv->vg->name) + strlen(lv->name) + 2;
	char *uuid = alloca(sizeof(lv->lvid));
	char *lv_full_name = alloca(len);

	if (!uuid || !lv_full_name)
		return_0;

	if (dm_snprintf(lv_full_name, len, "%s/%s", lv->vg->name, lv->name) < 0)
		return_0;

	memcpy(uuid, &lv->lvid, sizeof(lv->lvid));

	if (!lv_is_merging_origin(lv))
		return poll_daemon(cmd, lv_full_name, uuid, background, 0,
				   &_lvconvert_mirror_fns, "Converted");
	else
		return poll_daemon(cmd, lv_full_name, uuid, background, 0,
				   &_lvconvert_merge_fns, "Merged");
}

static int _insert_lvconvert_layer(struct cmd_context *cmd,
				   struct logical_volume *lv)
{
	char *format, *layer_name;
	size_t len;
	int i;

	/*
 	 * We would like to give the same number for this layer
 	 * and the newly added mimage.
 	 * However, LV name of newly added mimage is determined *after*
	 * the LV name of this layer is determined.
	 *
	 * So, use generate_lv_name() to generate mimage name first
	 * and take the number from it.
	 */

	len = strlen(lv->name) + 32;
	if (!(format = alloca(len)) ||
	    !(layer_name = alloca(len)) ||
	    dm_snprintf(format, len, "%s_mimage_%%d", lv->name) < 0) {
		log_error("lvconvert: layer name allocation failed.");
		return 0;
	}

	if (!generate_lv_name(lv->vg, format, layer_name, len) ||
	    sscanf(layer_name, format, &i) != 1) {
		log_error("lvconvert: layer name generation failed.");
		return 0;
	}

	if (dm_snprintf(layer_name, len, MIRROR_SYNC_LAYER "_%d", i) < 0) {
		log_error("layer name allocation failed.");
		return 0;
	}

	if (!insert_layer_for_lv(cmd, lv, 0, layer_name)) {
		log_error("Failed to insert resync layer");
		return 0;
	}

	return 1;
}

static int _failed_mirrors_count(struct logical_volume *lv)
{
	struct lv_segment *lvseg;
	int ret = 0;
	unsigned s;

	dm_list_iterate_items(lvseg, &lv->segments) {
		if (!seg_is_mirrored(lvseg))
			return -1;
		for (s = 0; s < lvseg->area_count; s++) {
			if (seg_type(lvseg, s) == AREA_LV) {
				if (is_temporary_mirror_layer(seg_lv(lvseg, s)))
					ret += _failed_mirrors_count(seg_lv(lvseg, s));
				else if (seg_lv(lvseg, s)->status & PARTIAL_LV)
					++ ret;
				else if (seg_type(lvseg, s) == AREA_PV &&
					 is_missing_pv(seg_pv(lvseg, s)))
					++ret;
			}
		}
	}

	return ret;
}

static int _failed_logs_count(struct logical_volume *lv)
{
	int ret = 0, s;
	struct logical_volume *log_lv = first_seg(lv)->log_lv;
	if (log_lv && (log_lv->status & PARTIAL_LV)) {
		if (log_lv->status & MIRRORED)
			ret += _failed_mirrors_count(log_lv);
		else
			ret += 1;
	}
	for (s = 0; s < first_seg(lv)->area_count; s++) {
		if (seg_type(first_seg(lv), s) == AREA_LV &&
		    is_temporary_mirror_layer(seg_lv(first_seg(lv), s)))
			ret += _failed_logs_count(seg_lv(first_seg(lv), s));
	}
	return ret;
}


static struct dm_list *_failed_pv_list(struct volume_group *vg)
{
	struct dm_list *failed_pvs;
	struct pv_list *pvl, *new_pvl;

	if (!(failed_pvs = dm_pool_alloc(vg->vgmem, sizeof(*failed_pvs)))) {
		log_error("Allocation of list of failed_pvs failed.");
		return_NULL;
	}

	dm_list_init(failed_pvs);

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!is_missing_pv(pvl->pv))
			continue;

		/* 
		 * Finally, --repair will remove empty PVs.
		 * But we only want remove these which are output of repair,
		 * Do not count these which are already empty here.
		 * FIXME: code should traverse PV in LV not in whole VG.
		 * FIXME: layer violation? should it depend on vgreduce --removemising?
		 */
		if (pvl->pv->pe_alloc_count == 0)
			continue;

		if (!(new_pvl = dm_pool_alloc(vg->vgmem, sizeof(*new_pvl)))) {
			log_error("Allocation of failed_pvs list entry failed.");
			return_NULL;
		}
		new_pvl->pv = pvl->pv;
		dm_list_add(failed_pvs, &new_pvl->list);
	}

	return failed_pvs;
}

static int _is_partial_lv(struct logical_volume *lv,
			  void *baton __attribute__((unused)))
{
	return lv->status & PARTIAL_LV;
}

/*
 * Walk down the stacked mirror LV to the original mirror LV.
 */
static struct logical_volume *_original_lv(struct logical_volume *lv)
{
	struct logical_volume *next_lv = lv, *tmp_lv;

	while ((tmp_lv = find_temporary_mirror(next_lv)))
		next_lv = tmp_lv;

	return next_lv;
}

static void _lvconvert_mirrors_repair_ask(struct cmd_context *cmd,
					  int failed_log, int failed_mirrors,
					  int *replace_log, int *replace_mirrors)
{
	const char *leg_policy = NULL, *log_policy = NULL;

	int force = arg_count(cmd, force_ARG);
	int yes = arg_count(cmd, yes_ARG);

	*replace_log = *replace_mirrors = 1;

	if (arg_count(cmd, use_policies_ARG)) {
		leg_policy = find_config_tree_str(cmd,
					"activation/mirror_image_fault_policy", NULL);
		if (!leg_policy)
			leg_policy = find_config_tree_str(cmd,
					"activation/mirror_device_fault_policy",
					DEFAULT_MIRROR_DEVICE_FAULT_POLICY);
		log_policy = find_config_tree_str(cmd,
					"activation/mirror_log_fault_policy",
					DEFAULT_MIRROR_LOG_FAULT_POLICY);
		*replace_mirrors = strcmp(leg_policy, "remove");
		*replace_log = strcmp(log_policy, "remove");
		return;
	}

	if (yes)
		return;

	if (force != PROMPT) {
		*replace_log = *replace_mirrors = 0;
		return;
	}

	if (failed_log &&
	    yes_no_prompt("Attempt to replace failed mirror log? [y/n]: ") == 'n') {
		*replace_log = 0;
	}

	if (failed_mirrors &&
	    yes_no_prompt("Attempt to replace failed mirror images "
			  "(requires full device resync)? [y/n]: ") == 'n') {
		*replace_mirrors = 0;
	}
}

/*
 * _get_log_count
 * @lv: the mirror LV
 *
 * Get the number of on-disk copies of the log.
 *  0  = 'core'
 *  1  = 'disk'
 *  2+ = 'mirrored'
 */
static int _get_log_count(struct logical_volume *lv)
{
	struct logical_volume *log_lv;

	log_lv = first_seg(_original_lv(lv))->log_lv;
	if (log_lv)
		return lv_mirror_count(log_lv);

	return 0;
}

static int _lv_update_mirrored_log(struct logical_volume *lv,
				   struct dm_list *operable_pvs,
				   int log_count)
{
	int old_log_count;
	struct logical_volume *log_lv;

	/*
	 * When log_count is 0, mirrored log doesn't need to be
	 * updated here but it will be removed later.
	 */
	if (!log_count)
		return 1;

	log_lv = first_seg(_original_lv(lv))->log_lv;
	if (!log_lv || !(log_lv->status & MIRRORED))
		return 1;

	old_log_count = _get_log_count(lv);
	if (old_log_count == log_count)
		return 1;

	/* Reducing redundancy of the log */
	return remove_mirror_images(log_lv, log_count,
				    is_mirror_image_removable,
				    operable_pvs, 0U);
}

static int _reload_lv(struct cmd_context *cmd, struct logical_volume *lv);
static int _lv_update_log_type(struct cmd_context *cmd,
			       struct lvconvert_params *lp,
			       struct logical_volume *lv,
			       struct dm_list *operable_pvs,
			       int log_count)
{
	uint32_t region_size;
	int old_log_count;
	struct logical_volume *original_lv;
	struct logical_volume *log_lv;

	old_log_count = _get_log_count(lv);
	if (old_log_count == log_count)
		return 1;

	original_lv = _original_lv(lv);
	/* Remove an existing log completely */
	if (!log_count) {
		if (!remove_mirror_log(cmd, original_lv, operable_pvs,
				       arg_count(cmd, yes_ARG) ||
				       arg_count(cmd, force_ARG)))
			return_0;
		return 1;
	}

	log_lv = first_seg(original_lv)->log_lv;

	/* Adding redundancy to the log */
	if (old_log_count < log_count) {

		region_size = adjusted_mirror_region_size(lv->vg->extent_size,
							  lv->le_count,
							  lp->region_size);

		if (!add_mirror_log(cmd, original_lv, log_count,
				    region_size, operable_pvs, lp->alloc))
			return_0;
		/*
		 * FIXME: This simple approach won't work in cluster mirrors,
		 *        but it doesn't matter because we don't support
		 *        mirrored logs in cluster mirrors.
		 */
		if (old_log_count)
			return _reload_lv(cmd, log_lv);
		return 1;
	}

	/* Reducing redundancy of the log */
	return remove_mirror_images(log_lv, log_count, is_mirror_image_removable, operable_pvs, 1U);
}

/*
 * Reomove missing and empty PVs from VG, if are also in provided list
 */
static void _remove_missing_empty_pv(struct volume_group *vg, struct dm_list *remove_pvs)
{
	struct pv_list *pvl, *pvl_vg, *pvlt;
	int removed = 0;

	if (!remove_pvs)
		return;

	dm_list_iterate_items(pvl, remove_pvs) {
		dm_list_iterate_items_safe(pvl_vg, pvlt, &vg->pvs) {
			if (!id_equal(&pvl->pv->id, &pvl_vg->pv->id) ||
			    !is_missing_pv(pvl_vg->pv) ||
			    pvl_vg->pv->pe_alloc_count != 0)
				continue;

			/* FIXME: duplication of vgreduce code, move this to library */
			vg->free_count -= pvl_vg->pv->pe_count;
			vg->extent_count -= pvl_vg->pv->pe_count;
			del_pvl_from_vgs(vg, pvl_vg);
			free_pv_fid(pvl_vg->pv);

			removed++;
		}
	}

	if (removed) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			stack;
			return;
		}
		log_warn("%d missing and now unallocated Physical Volumes removed from VG.", removed);
	}
}

/*
 * _lvconvert_mirrors_parse_params
 *
 * This function performs the following:
 *  1) Gets the old values of mimage and log counts
 *  2) Parses the CLI args to find the new desired values
 *  3) Adjusts 'lp->mirrors' to the appropriate absolute value.
 *     (Remember, 'lp->mirrors' is specified in terms of the number of "copies"
 *      vs. the number of mimages.  It can also be a relative value.)
 *  4) Sets 'lp->need_polling' if collapsing
 *  5) Validates other mirror params
 *
 * Returns: 1 on success, 0 on error
 */
static int _lvconvert_mirrors_parse_params(struct cmd_context *cmd,
					   struct logical_volume *lv,
					   struct lvconvert_params *lp,
					   uint32_t *old_mimage_count,
					   uint32_t *old_log_count,
					   uint32_t *new_mimage_count,
					   uint32_t *new_log_count)
{
	int repair = arg_count(cmd, repair_ARG);
	const char *mirrorlog;
	*old_mimage_count = lv_mirror_count(lv);
	*old_log_count = _get_log_count(lv);

	/*
	 * Collapsing a stack of mirrors:
	 *
	 * If called with no argument, try collapsing the resync layers
	 */
	if (!arg_count(cmd, mirrors_ARG) && !arg_count(cmd, mirrorlog_ARG) &&
	    !arg_count(cmd, corelog_ARG) && !arg_count(cmd, regionsize_ARG) &&
	    !arg_count(cmd, splitmirrors_ARG) && !repair) {
		*new_mimage_count = *old_mimage_count;
		*new_log_count = *old_log_count;

		if (find_temporary_mirror(lv) || (lv->status & CONVERTING))
			lp->need_polling = 1;
		return 1;
	}

	if ((arg_count(cmd, mirrors_ARG) && repair) ||
	    (arg_count(cmd, mirrorlog_ARG) && repair) ||
	    (arg_count(cmd, corelog_ARG) && repair)) {
		log_error("--repair cannot be used with --mirrors, --mirrorlog,"
			  " or --corelog");
		return 0;
	}

	if (arg_count(cmd, mirrorlog_ARG) && arg_count(cmd, corelog_ARG)) {
		log_error("--mirrorlog and --corelog are incompatible");
		return 0;
	}

	/*
	 * Adjusting mimage count?
	 */
	if (!arg_count(cmd, mirrors_ARG) && !arg_count(cmd, splitmirrors_ARG))
		lp->mirrors = *old_mimage_count;
	else if (lp->mirrors_sign == SIGN_PLUS)
		lp->mirrors = *old_mimage_count + lp->mirrors;
	else if (lp->mirrors_sign == SIGN_MINUS)
		lp->mirrors = (*old_mimage_count > lp->mirrors) ?
			*old_mimage_count - lp->mirrors: 0;
	else
		lp->mirrors += 1;

	*new_mimage_count = lp->mirrors;

	/* Too many mimages? */
	if (lp->mirrors > DEFAULT_MIRROR_MAX_IMAGES) {
		log_error("Only up to %d images in mirror supported currently.",
			  DEFAULT_MIRROR_MAX_IMAGES);
		return 0;
	}

	/* Did the user try to subtract more legs than available? */
	if (lp->mirrors < 1) {
		log_error("Unable to reduce images by specified amount - only %d in %s",
			  *old_mimage_count, lv->name);
		return 0;
	}

	/*
	 * FIXME: It would be nice to say what we are adjusting to, but
	 * I really don't know whether to specify the # of copies or mimages.
	 */
	if (*old_mimage_count != *new_mimage_count)
		log_verbose("Adjusting mirror image count of %s", lv->name);

	/*
	 * Adjust log type
	 *
	 * If we are converting from a mirror to another mirror or simply
	 * changing the log type, we start by assuming they want the log
	 * type the same and then parse the given args.  OTOH, If we are
	 * converting from linear to mirror, then we start from the default
	 * position that the user would like a 'disk' log.
	 */
	*new_log_count = (*old_mimage_count > 1) ? *old_log_count : 1;
	if (!arg_count(cmd, corelog_ARG) && !arg_count(cmd, mirrorlog_ARG))
		return 1;

	if (arg_count(cmd, corelog_ARG))
		*new_log_count = 0;

	mirrorlog = arg_str_value(cmd, mirrorlog_ARG,
				  !*new_log_count ? "core" : DEFAULT_MIRRORLOG);

	if (!strcmp("mirrored", mirrorlog))
		*new_log_count = 2;
	else if (!strcmp("disk", mirrorlog))
		*new_log_count = 1;
	else if (!strcmp("core", mirrorlog))
		*new_log_count = 0;
	else {
		log_error("Unknown mirrorlog type: %s", mirrorlog);
		return 0;
	}

	/*
	 * No mirrored logs for cluster mirrors until
	 * log daemon is multi-threaded.
	 */
	if ((*new_log_count == 2) && vg_is_clustered(lv->vg)) {
		log_error("Log type, \"mirrored\", is unavailable to cluster mirrors");
		return 0;
	}

	log_verbose("Setting logging type to %s", mirrorlog);

	/*
	 * Region size must not change on existing mirrors
	 */
	if (arg_count(cmd, regionsize_ARG) && (lv->status & MIRRORED) &&
	    (lp->region_size != first_seg(lv)->region_size)) {
		log_error("Mirror log region size cannot be changed on "
			  "an existing mirror.");
		return 0;
	}

	/*
	 * For the most part, we cannot handle multi-segment mirrors. Bail out
	 * early if we have encountered one.
	 */
	if ((lv->status & MIRRORED) && dm_list_size(&lv->segments) != 1) {
		log_error("Logical volume %s has multiple "
			  "mirror segments.", lv->name);
		return 0;
	}

	return 1;
}

static int _reload_lv(struct cmd_context *cmd, struct logical_volume *lv)
{
	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	if (!vg_write(lv->vg))
		return_0;

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		if (!resume_lv(cmd, lv))
			stack;
		return_0;
	}

	log_very_verbose("Updating \"%s\" in kernel", lv->name);

	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}
	return 1;
}

/*
 * _lvconvert_mirrors_aux
 *
 * Add/remove mirror images and adjust log type.  'operable_pvs'
 * are the set of PVs open to removal or allocation - depending
 * on the operation being performed.
 */
static int _lvconvert_mirrors_aux(struct cmd_context *cmd,
				  struct logical_volume *lv,
				  struct lvconvert_params *lp,
				  struct dm_list *operable_pvs,
				  uint32_t new_mimage_count,
				  uint32_t new_log_count)
{
	uint32_t region_size;
	struct lv_segment *seg;
	struct logical_volume *layer_lv;
	uint32_t old_mimage_count = lv_mirror_count(lv);
	uint32_t old_log_count = _get_log_count(lv);

	if ((lp->mirrors == 1) && !(lv->status & MIRRORED)) {
		log_error("Logical volume %s is already not mirrored.",
			  lv->name);
		return 1;
	}

	region_size = adjusted_mirror_region_size(lv->vg->extent_size,
						  lv->le_count,
						  lp->region_size);

	if (!operable_pvs)
		operable_pvs = lp->pvh;

	seg = first_seg(lv);

	/*
	 * Up-convert from linear to mirror
	 */
	if (!(lv->status & MIRRORED)) {
		/* FIXME Share code with lvcreate */

		/*
		 * FIXME should we give not only lp->pvh, but also all PVs
		 * currently taken by the mirror? Would make more sense from
		 * user perspective.
		 */
		if (!lv_add_mirrors(cmd, lv, new_mimage_count - 1, lp->stripes,
				    lp->stripe_size, region_size, new_log_count, operable_pvs,
				    lp->alloc, MIRROR_BY_LV))
			return_0;

		if (lp->wait_completion)
			lp->need_polling = 1;

		goto out;
	}

	/*
	 * Up-convert m-way mirror to n-way mirror
	 */
	if (new_mimage_count > old_mimage_count) {
		if (lv->status & LV_NOTSYNCED) {
			log_error("Can't add mirror to out-of-sync mirrored "
				  "LV: use lvchange --resync first.");
			return 0;
		}

		/*
		 * We allow snapshots of mirrors, but for now, we
		 * do not allow up converting mirrors that are under
		 * snapshots.  The layering logic is somewhat complex,
		 * and preliminary test show that the conversion can't
		 * seem to get the correct %'age of completion.
		 */
		if (lv_is_origin(lv)) {
			log_error("Can't add additional mirror images to "
				  "mirrors that are under snapshots");
			return 0;
		}

		/*
		 * Is there already a convert in progress?  We do not
		 * currently allow more than one.
		 */
		if (find_temporary_mirror(lv) || (lv->status & CONVERTING)) {
			log_error("%s is already being converted.  Unable to start another conversion.",
				  lv->name);
			return 0;
		}

		/*
		 * Log addition/removal should be done before the layer
		 * insertion to make the end result consistent with
		 * linear-to-mirror conversion.
		 */
		if (!_lv_update_log_type(cmd, lp, lv,
					 operable_pvs, new_log_count)) {
			stack;
			return 0;
		}

		/* Insert a temporary layer for syncing,
		 * only if the original lv is using disk log. */
		if (seg->log_lv && !_insert_lvconvert_layer(cmd, lv)) {
			log_error("Failed to insert resync layer");
			return 0;
		}

		/* FIXME: can't have multiple mlogs. force corelog. */
		if (!lv_add_mirrors(cmd, lv,
				    new_mimage_count - old_mimage_count,
				    lp->stripes, lp->stripe_size,
				    region_size, 0U, operable_pvs, lp->alloc,
				    MIRROR_BY_LV)) {
			layer_lv = seg_lv(first_seg(lv), 0);
			if (!remove_layer_from_lv(lv, layer_lv) ||
			    !deactivate_lv(cmd, layer_lv) ||
			    !lv_remove(layer_lv) || !vg_write(lv->vg) ||
			    !vg_commit(lv->vg)) {
				log_error("ABORTING: Failed to remove "
					  "temporary mirror layer %s.",
					  layer_lv->name);
				log_error("Manual cleanup with vgcfgrestore "
					  "and dmsetup may be required.");
				return 0;
			}
			stack;
			return 0;
		}
		if (seg->log_lv)
			lv->status |= CONVERTING;
		lp->need_polling = 1;

		goto out_skip_log_convert;
	}

	/*
	 * Down-convert (reduce # of mimages).
	 */
	if (new_mimage_count < old_mimage_count) {
		uint32_t nmc = old_mimage_count - new_mimage_count;
		uint32_t nlc = (!new_log_count || lp->mirrors == 1) ? 1U : 0U;

		/* FIXME: Why did nlc used to be calculated that way? */

		/* Reduce number of mirrors */
		if (lp->keep_mimages) {
			if (arg_count(cmd, trackchanges_ARG)) {
				log_error("--trackchanges is not available "
					  "to 'mirror' segment type");
				return 0;
			}
			if (!lv_split_mirror_images(lv, lp->lv_split_name,
						    nmc, operable_pvs))
				return 0;
		} else if (!lv_remove_mirrors(cmd, lv, nmc, nlc,
					      is_mirror_image_removable, operable_pvs, 0))
			return_0;

		goto out; /* Just in case someone puts code between */
	}

out:
	/*
	 * Converting the log type
	 */
	if ((lv->status & MIRRORED) && (old_log_count != new_log_count)) {
		if (!_lv_update_log_type(cmd, lp, lv,
					 operable_pvs, new_log_count)) {
			stack;
			return 0;
		}
	}

out_skip_log_convert:

	if (!_reload_lv(cmd, lv))
		return 0;

	return 1;
}

int mirror_remove_missing(struct cmd_context *cmd,
			  struct logical_volume *lv, int force)
{
	struct dm_list *failed_pvs;
	int log_count = _get_log_count(lv) - _failed_logs_count(lv);

	if (!(failed_pvs = _failed_pv_list(lv->vg)))
		return_0;

	/* No point in keeping a log if the result is not a mirror */
	if (_failed_mirrors_count(lv) + 1 >= lv_mirror_count(lv))
		log_count = 0;

        if (force && _failed_mirrors_count(lv) == lv_mirror_count(lv)) {
		log_error("No usable images left in %s.", lv->name);
		return lv_remove_with_dependencies(cmd, lv, 1, 0);
        }

	/*
	 * We must adjust the log first, or the entire mirror
	 * will get stuck during a suspend.
	 */
	if (!_lv_update_mirrored_log(lv, failed_pvs, log_count))
		return 0;

	if (_failed_mirrors_count(lv) > 0 &&
	    !lv_remove_mirrors(cmd, lv, _failed_mirrors_count(lv),
			       log_count ? 0U : 1U,
			       _is_partial_lv, NULL, 0))
		return 0;

	if (!_lv_update_log_type(cmd, NULL, lv, failed_pvs,
				 log_count))
		return 0;

	if (!_reload_lv(cmd, lv))
		return 0;

	return 1;
}

/*
 * _lvconvert_mirrors_repair
 *
 * This function operates in two phases.  First, all of the bad
 * devices are removed from the mirror.  Then, if desired by the
 * user, the devices are replaced.
 *
 * 'old_mimage_count' and 'old_log_count' are there so we know
 * what to convert to after the removal of devices.
 */
static int _lvconvert_mirrors_repair(struct cmd_context *cmd,
				     struct logical_volume *lv,
				     struct lvconvert_params *lp)
{
	int failed_logs = 0;
	int failed_mimages = 0;
	int replace_logs = 0;
	int replace_mimages = 0;
	uint32_t log_count;

	uint32_t original_mimages = lv_mirror_count(lv);
	uint32_t original_logs = _get_log_count(lv);

	cmd->handles_missing_pvs = 1;
	cmd->partial_activation = 1;
	lp->need_polling = 0;

	lv_check_transient(lv); /* TODO check this in lib for all commands? */

	if (!(lv->status & PARTIAL_LV)) {
		log_error("%s is consistent. Nothing to repair.", lv->name);
		return 1;
	}

	failed_mimages = _failed_mirrors_count(lv);
	failed_logs = _failed_logs_count(lv);

	mirror_remove_missing(cmd, lv, 0);

	if (failed_mimages)
		log_error("Mirror status: %d of %d images failed.",
			  failed_mimages, original_mimages);

	/*
	 * Count the failed log devices
	 */
	if (failed_logs)
		log_error("Mirror log status: %d of %d images failed.",
			  failed_logs, original_logs);

	/*
	 * Find out our policies
	 */
	_lvconvert_mirrors_repair_ask(cmd, failed_logs, failed_mimages,
				      &replace_logs, &replace_mimages);

	/*
	 * Second phase - replace faulty devices
	 */
	lp->mirrors = replace_mimages ? original_mimages : (original_mimages - failed_mimages);

	/*
	 * It does not make sense to replace the log if the volume is no longer
	 * a mirror.
	 */
	if (lp->mirrors == 1)
		replace_logs = 0;

	log_count = replace_logs ? original_logs : (original_logs - failed_logs);

	while (replace_mimages || replace_logs) {
		log_warn("Trying to up-convert to %d images, %d logs.", lp->mirrors, log_count);
		if (_lvconvert_mirrors_aux(cmd, lv, lp, NULL,
					   lp->mirrors, log_count))
			break;
		else {
			if (lp->mirrors > 2)
				-- lp->mirrors;
			else if (log_count > 0)
				-- log_count;
			else
				break; /* nowhere to go, anymore... */
		}
	}

	if (replace_mimages && lv_mirror_count(lv) != original_mimages)
		log_warn("WARNING: Failed to replace %d of %d images in volume %s",
			 original_mimages - lv_mirror_count(lv), original_mimages, lv->name);
	if (replace_logs && _get_log_count(lv) != original_logs)
		log_warn("WARNING: Failed to replace %d of %d logs in volume %s",
			 original_logs - _get_log_count(lv), original_logs, lv->name);

	/* if (!arg_count(cmd, use_policies_ARG) && (lp->mirrors != old_mimage_count
						  || log_count != old_log_count))
						  return 0; */

	return 1;
}

/*
 * _lvconvert_mirrors
 *
 * Determine what is being done.  Are we doing a conversion, repair, or
 * collapsing a stack?  Once determined, call helper functions.
 */
static int _lvconvert_mirrors(struct cmd_context *cmd,
			      struct logical_volume *lv,
			      struct lvconvert_params *lp)
{
	int repair = arg_count(cmd, repair_ARG);
	uint32_t old_mimage_count;
	uint32_t old_log_count;
	uint32_t new_mimage_count;
	uint32_t new_log_count;

	if (lp->merge_mirror) {
		log_error("Unable to merge mirror images"
			  "of segment type 'mirror'");
		return 0;
	}

	/* Adjust mimage and/or log count */
	if (!_lvconvert_mirrors_parse_params(cmd, lv, lp,
					     &old_mimage_count, &old_log_count,
					     &new_mimage_count, &new_log_count))
		return 0;

        if (((old_mimage_count < new_mimage_count && old_log_count > new_log_count) ||
             (old_mimage_count > new_mimage_count && old_log_count < new_log_count)) &&
            lp->pv_count) {
		log_error("Cannot both allocate and free extents when "
			  "specifying physical volumes to use.");
		log_error("Please specify the operation in two steps.");
		return 0;
        }

	/* Nothing to do?  (Probably finishing collapse.) */
	if ((old_mimage_count == new_mimage_count) &&
	    (old_log_count == new_log_count) && !repair)
		return 1;

	if (repair)
		return _lvconvert_mirrors_repair(cmd, lv, lp);

	if (!_lvconvert_mirrors_aux(cmd, lv, lp, NULL,
				    new_mimage_count, new_log_count))
		return 0;

	if (!lp->need_polling)
		log_print("Logical volume %s converted.", lv->name);

	backup(lv->vg);
	return 1;
}

static int is_valid_raid_conversion(const struct segment_type *from_segtype,
				    const struct segment_type *to_segtype)
{
	if (from_segtype == to_segtype)
		return 1;

	if (!segtype_is_raid(from_segtype) && !segtype_is_raid(to_segtype))
		return_0;  /* Not converting to or from RAID? */

	return 1;
}

static void _lvconvert_raid_repair_ask(struct cmd_context *cmd, int *replace_dev)
{
	const char *dev_policy = NULL;

	int force = arg_count(cmd, force_ARG);
	int yes = arg_count(cmd, yes_ARG);

	*replace_dev = 0;

	if (arg_count(cmd, use_policies_ARG)) {
		dev_policy = find_config_tree_str(cmd, "activation/raid_fault_policy", DEFAULT_RAID_FAULT_POLICY);

		if (!strcmp(dev_policy, "allocate") ||
		    !strcmp(dev_policy, "replace"))
			*replace_dev = 1;
		/* else if (!strcmp(dev_policy, "anything_else")) -- ignore */
		return;
	}

	if (yes) {
		*replace_dev = 1;
		return;
	}

	if (force != PROMPT)
		return;

	if (yes_no_prompt("Attempt to replace failed RAID images "
			  "(requires full device resync)? [y/n]: ") == 'y') {
		*replace_dev = 1;
	}
}

static int lvconvert_raid(struct logical_volume *lv, struct lvconvert_params *lp)
{
	int replace = 0;
	int uninitialized_var(image_count);
	struct dm_list *failed_pvs;
	struct cmd_context *cmd = lv->vg->cmd;
	struct lv_segment *seg = first_seg(lv);

	if (!arg_count(cmd, type_ARG))
		lp->segtype = seg->segtype;

	/* Can only change image count for raid1 and linear */
	if (arg_count(cmd, mirrors_ARG) &&
	    !seg_is_mirrored(seg) && !seg_is_linear(seg)) {
		log_error("'--mirrors/-m' is not compatible with %s",
			  seg->segtype->name);
		return 0;
	}

	if (!is_valid_raid_conversion(seg->segtype, lp->segtype)) {
		log_error("Unable to convert %s/%s from %s to %s",
			  lv->vg->name, lv->name,
			  seg->segtype->name, lp->segtype->name);
		return 0;
	}

	/* Change number of RAID1 images */
	if (arg_count(cmd, mirrors_ARG) || arg_count(cmd, splitmirrors_ARG)) {
		image_count = lv_raid_image_count(lv);
		if (lp->mirrors_sign == SIGN_PLUS)
			image_count += lp->mirrors;
		else if (lp->mirrors_sign == SIGN_MINUS)
			image_count -= lp->mirrors;
		else
			image_count = lp->mirrors + 1;

		if (image_count < 1) {
			log_error("Unable to %s images by specified amount",
				  arg_count(cmd, splitmirrors_ARG) ?
				  "split" : "reduce");
			return 0;
		}
	}

	if (lp->merge_mirror)
		return lv_raid_merge(lv);

	if (arg_count(cmd, trackchanges_ARG))
		return lv_raid_split_and_track(lv, lp->pvh);

	if (arg_count(cmd, splitmirrors_ARG))
		return lv_raid_split(lv, lp->lv_split_name,
				     image_count, lp->pvh);

	if (arg_count(cmd, mirrors_ARG))
		return lv_raid_change_image_count(lv, image_count, lp->pvh);

	if (arg_count(cmd, type_ARG))
		return lv_raid_reshape(lv, lp->segtype);

	if (arg_count(cmd, replace_ARG))
		return lv_raid_replace(lv, lp->replace_pvh, lp->pvh);

	if (arg_count(cmd, repair_ARG)) {
		_lvconvert_raid_repair_ask(cmd, &replace);

		if (replace) {
			if (!(failed_pvs = _failed_pv_list(lv->vg))) {
				stack;
				return ECMD_FAILED;
			}
			return lv_raid_replace(lv, failed_pvs, lp->pvh);
		}

		/* "warn" if policy not set to replace */
		if (arg_count(cmd, use_policies_ARG))
			log_error("Issue 'lvconvert --repair %s/%s' to "
				  "replace failed device",
				  lv->vg->name, lv->name);
		return 1;
	}

	log_error("Conversion operation not yet supported.");
	return 0;
}

static int lvconvert_snapshot(struct cmd_context *cmd,
			      struct logical_volume *lv,
			      struct lvconvert_params *lp)
{
	struct logical_volume *org;
	int r = 0;

	if (!(org = find_lv(lv->vg, lp->origin))) {
		log_error("Couldn't find origin volume '%s'.", lp->origin);
		return 0;
	}

	if (org == lv) {
		log_error("Unable to use \"%s\" as both snapshot and origin.",
			  lv->name);
		return 0;
	}

	if (org->status & (LOCKED|PVMOVE|MIRRORED) || lv_is_cow(org)) {
		log_error("Unable to convert an LV into a snapshot of a %s LV.",
			  org->status & LOCKED ? "locked" :
			  org->status & PVMOVE ? "pvmove" :
			  org->status & MIRRORED ? "mirrored" :
			  "snapshot");
		return 0;
	}

	if (!lp->zero || !(lv->status & LVM_WRITE))
		log_warn("WARNING: \"%s\" not zeroed", lv->name);
	else if (!set_lv(cmd, lv, UINT64_C(0), 0)) {
		log_error("Aborting. Failed to wipe snapshot "
			  "exception store.");
		return 0;
	}

	if (!deactivate_lv(cmd, lv)) {
		log_error("Couldn't deactivate LV %s.", lv->name);
		return 0;
	}

	if (!vg_add_snapshot(org, lv, NULL, org->le_count, lp->chunk_size)) {
		log_error("Couldn't create snapshot.");
		return 0;
	}

	/* store vg on disk(s) */
	if (!vg_write(lv->vg))
		return_0;

	if (!suspend_lv(cmd, org)) {
		log_error("Failed to suspend origin %s", org->name);
		vg_revert(lv->vg);
		goto out;
	}

	if (!vg_commit(lv->vg))
		goto_out;

	if (!resume_lv(cmd, org)) {
		log_error("Problem reactivating origin %s", org->name);
		goto out;
	}

	log_print("Logical volume %s converted to snapshot.", lv->name);
	r = 1;
out:
	backup(lv->vg);
	return r;
}

static int lvconvert_merge(struct cmd_context *cmd,
			   struct logical_volume *lv,
			   struct lvconvert_params *lp)
{
	int r = 0;
	int merge_on_activate = 0;
	struct logical_volume *origin = origin_from_cow(lv);
	struct lv_segment *cow_seg = find_cow(lv);
	struct lvinfo info;

	/* Check if merge is possible */
	if (lv_is_merging_cow(lv)) {
		log_error("Snapshot %s is already merging", lv->name);
		return 0;
	}
	if (lv_is_merging_origin(origin)) {
		log_error("Snapshot %s is already merging into the origin",
			  find_merging_cow(origin)->cow->name);
		return 0;
	}

	/*
	 * Prevent merge with open device(s) as it would likely lead
	 * to application/filesystem failure.  Merge on origin's next
	 * activation if either the origin or snapshot LV are currently
	 * open.
	 *
	 * FIXME testing open_count is racey; snapshot-merge target's
	 * constructor and DM should prevent appropriate devices from
	 * being open.
	 */
	if (lv_info(cmd, origin, 0, &info, 1, 0)) {
		if (info.open_count) {
			log_error("Can't merge over open origin volume");
			merge_on_activate = 1;
		}
	}
	if (lv_info(cmd, lv, 0, &info, 1, 0)) {
		if (info.open_count) {
			log_print("Can't merge when snapshot is open");
			merge_on_activate = 1;
		}
	}

	init_snapshot_merge(cow_seg, origin);

	/* store vg on disk(s) */
	if (!vg_write(lv->vg))
		return_0;

	if (merge_on_activate) {
		/* commit vg but skip starting the merge */
		if (!vg_commit(lv->vg))
			return_0;
		r = 1;
		log_print("Merging of snapshot %s will start "
			  "next activation.", lv->name);
		goto out;
	}

	/* Perform merge */
	if (!suspend_lv(cmd, origin)) {
		log_error("Failed to suspend origin %s", origin->name);
		vg_revert(lv->vg);
		goto out;
	}

	if (!vg_commit(lv->vg)) {
		if (!resume_lv(cmd, origin))
			stack;
		goto_out;
	}

	if (!resume_lv(cmd, origin)) {
		log_error("Failed to reactivate origin %s", origin->name);
		goto out;
	}

	lp->need_polling = 1;
	lp->lv_to_poll = origin;

	r = 1;
	log_print("Merging of volume %s started.", lv->name);
out:
	backup(lv->vg);
	return r;
}

static int _lvconvert_single(struct cmd_context *cmd, struct logical_volume *lv,
			     void *handle)
{
	struct lvconvert_params *lp = handle;
	struct dm_list *failed_pvs;
	struct lvinfo info;
	percent_t snap_percent;

	if (lv->status & LOCKED) {
		log_error("Cannot convert locked LV %s", lv->name);
		return ECMD_FAILED;
	}

	if (lv_is_cow(lv) && !lp->merge) {
		log_error("Can't convert snapshot logical volume \"%s\"",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv->status & PVMOVE) {
		log_error("Unable to convert pvmove LV %s", lv->name);
		return ECMD_FAILED;
	}

	if (arg_count(cmd, repair_ARG) && !(lv->status & MIRRORED)) {
		if (arg_count(cmd, use_policies_ARG))
			return ECMD_PROCESSED; /* nothing to be done here */
		log_error("Can't repair non-mirrored LV \"%s\".", lv->name);
		return ECMD_FAILED;
	}

	if (!lp->segtype)
		lp->segtype = first_seg(lv)->segtype;

	if (lp->merge) {
		if (!lv_is_cow(lv)) {
			log_error("Logical volume \"%s\" is not a snapshot",
				  lv->name);
			return ECMD_FAILED;
		}
	        if (lv_info(lv->vg->cmd, lv, 0, &info, 1, 0)
		    && info.exists && info.live_table &&
		    (!lv_snapshot_percent(lv, &snap_percent) ||
		     snap_percent == PERCENT_INVALID)) {
			log_error("Unable to merge invalidated snapshot LV \"%s\"", lv->name);
			return ECMD_FAILED;
		}
		if (!archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		if (!lvconvert_merge(cmd, lv, lp)) {
			log_error("Unable to merge LV \"%s\" into its origin.", lv->name);
			return ECMD_FAILED;
		}
	} else if (lp->snapshot) {
		if (lv->status & MIRRORED) {
			log_error("Unable to convert mirrored LV \"%s\" into a snapshot.", lv->name);
			return ECMD_FAILED;
		}
		if (!archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		if (!lvconvert_snapshot(cmd, lv, lp)) {
			stack;
			return ECMD_FAILED;
		}
	} else if (segtype_is_raid(lp->segtype) ||
		   (lv->status & RAID) || lp->merge_mirror) {
		if (!archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		if (!lvconvert_raid(lv, lp)) {
			stack;
			return ECMD_FAILED;
		}

		if (!(failed_pvs = _failed_pv_list(lv->vg))) {
			stack;
			return ECMD_FAILED;
		}

		/* If repairing and using policies, remove missing PVs from VG */
		if (arg_count(cmd, repair_ARG) && arg_count(cmd, use_policies_ARG))
			_remove_missing_empty_pv(lv->vg, failed_pvs);
	} else if (arg_count(cmd, mirrors_ARG) ||
		   arg_count(cmd, splitmirrors_ARG) ||
		   (lv->status & MIRRORED)) {
		if (!archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		if (!_lvconvert_mirrors(cmd, lv, lp)) {
			stack;
			return ECMD_FAILED;
		}

		if (!(failed_pvs = _failed_pv_list(lv->vg))) {
			stack;
			return ECMD_FAILED;
		}

		/* If repairing and using policies, remove missing PVs from VG */
		if (arg_count(cmd, repair_ARG) && arg_count(cmd, use_policies_ARG))
			_remove_missing_empty_pv(lv->vg, failed_pvs);
	}

	return ECMD_PROCESSED;
}

/*
 * FIXME move to toollib along with the rest of the drop/reacquire
 * VG locking that is used by lvconvert_merge_single()
 */
static struct logical_volume *get_vg_lock_and_logical_volume(struct cmd_context *cmd,
							     const char *vg_name,
							     const char *lv_name)
{
	/*
	 * Returns NULL if the requested LV doesn't exist;
	 * otherwise the caller must release_vg(lv->vg)
	 * - it is also up to the caller to unlock_vg() as needed
	 */
	struct volume_group *vg;
	struct logical_volume* lv = NULL;

	vg = _get_lvconvert_vg(cmd, vg_name, NULL);
	if (vg_read_error(vg)) {
		release_vg(vg);
		return_NULL;
	}

	if (!(lv = _get_lvconvert_lv(cmd, vg, lv_name, NULL, 0))) {
		log_error("Can't find LV %s in VG %s", lv_name, vg_name);
		unlock_and_release_vg(cmd, vg, vg_name);
		return NULL;
	}

	return lv;
}

static int poll_logical_volume(struct cmd_context *cmd, struct logical_volume *lv,
			       int wait_completion)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, 0, &info, 0, 0) || !info.exists) {
		log_print("Conversion starts after activation.");
		return ECMD_PROCESSED;
	}
	return lvconvert_poll(cmd, lv, wait_completion ? 0 : 1U);
}

static int lvconvert_single(struct cmd_context *cmd, struct lvconvert_params *lp)
{
	struct logical_volume *lv = NULL;
	int ret = ECMD_FAILED;
	int saved_ignore_suspended_devices = ignore_suspended_devices();

	if (arg_count(cmd, repair_ARG)) {
		init_ignore_suspended_devices(1);
		cmd->handles_missing_pvs = 1;
	}

	lv = get_vg_lock_and_logical_volume(cmd, lp->vg_name, lp->lv_name);
	if (!lv)
		goto_out;

	/*
	 * lp->pvh holds the list of PVs available for allocation or removal
	 */
	if (lp->pv_count) {
		if (!(lp->pvh = create_pv_list(cmd->mem, lv->vg, lp->pv_count,
					      lp->pvs, 0)))
			goto_bad;
	} else
		lp->pvh = &lv->vg->pvs;

	if (lp->replace_pv_count &&
	    !(lp->replace_pvh = create_pv_list(cmd->mem, lv->vg,
					       lp->replace_pv_count,
					       lp->replace_pvs, 0)))
			goto_bad;

	lp->lv_to_poll = lv;
	ret = _lvconvert_single(cmd, lv, lp);
bad:
	unlock_vg(cmd, lp->vg_name);

	if (ret == ECMD_PROCESSED && lp->need_polling)
		ret = poll_logical_volume(cmd, lp->lv_to_poll,
					  lp->wait_completion);

	release_vg(lv->vg);
out:
	init_ignore_suspended_devices(saved_ignore_suspended_devices);
	return ret;
}

static int lvconvert_merge_single(struct cmd_context *cmd, struct logical_volume *lv,
				  void *handle)
{
	struct lvconvert_params *lp = handle;
	const char *vg_name = NULL;
	struct logical_volume *refreshed_lv = NULL;
	int ret;

	/*
	 * FIXME can't trust lv's VG to be current given that caller
	 * is process_each_lv() -- poll_logical_volume() may have
	 * already updated the VG's metadata in an earlier iteration.
	 * - preemptively drop the VG lock, as is needed for
	 *   poll_logical_volume(), refresh LV (and VG in the process).
	 */
	vg_name = lv->vg->name;
	unlock_vg(cmd, vg_name);
	refreshed_lv = get_vg_lock_and_logical_volume(cmd, vg_name, lv->name);
	if (!refreshed_lv) {
		log_error("ABORTING: Can't reread LV %s/%s", vg_name, lv->name);
		return ECMD_FAILED;
	}

	lp->lv_to_poll = refreshed_lv;
	ret = _lvconvert_single(cmd, refreshed_lv, lp);

	if (ret == ECMD_PROCESSED && lp->need_polling) {
		/*
		 * Must drop VG lock, because lvconvert_poll() needs it,
		 * then reacquire it after polling completes
		 */
		unlock_vg(cmd, vg_name);

		ret = poll_logical_volume(cmd, lp->lv_to_poll,
					  lp->wait_completion);

		/* use LCK_VG_WRITE to match lvconvert()'s READ_FOR_UPDATE */
		if (!lock_vol(cmd, vg_name, LCK_VG_WRITE)) {
			log_error("ABORTING: Can't relock VG for %s "
				  "after polling finished", vg_name);
			ret = ECMD_FAILED;
		}
	}

	release_vg(refreshed_lv->vg);

	return ret;
}

int lvconvert(struct cmd_context * cmd, int argc, char **argv)
{
	struct lvconvert_params lp;

	if (!_read_params(&lp, cmd, argc, argv)) {
		stack;
		return EINVALID_CMD_LINE;
	}

	if (lp.merge) {
		if (!argc) {
			log_error("Please provide logical volume path");
			return EINVALID_CMD_LINE;
		}
		return process_each_lv(cmd, argc, argv, READ_FOR_UPDATE, &lp,
				       &lvconvert_merge_single);
	}

	return lvconvert_single(cmd, &lp);
}
