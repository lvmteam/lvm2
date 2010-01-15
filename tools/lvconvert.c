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

struct lvconvert_params {
	int snapshot;
	int merge;
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

	struct segment_type *segtype;

	alloc_policy_t alloc;

	int pv_count;
	char **pvs;
	struct dm_list *pvh;
	struct dm_list *failed_pvs;

	struct logical_volume *lv_to_poll;
};

static int _lvconvert_name_params(struct lvconvert_params *lp,
				  struct cmd_context *cmd,
				  int *pargc, char ***pargv)
{
	char *ptr;
	const char *vg_name = NULL;

	if (lp->snapshot && !lp->merge) {
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

	if (!apply_lvname_restrictions(lp->lv_name))
		return_0;

	if (*pargc && (lp->snapshot || lp->merge)) {
		log_error("Too many arguments provided for snapshots");
		return 0;
	}

	return 1;
}

static int _read_params(struct lvconvert_params *lp, struct cmd_context *cmd,
			int argc, char **argv)
{
	int region_size;
	int pagesize = lvm_getpagesize();

	memset(lp, 0, sizeof(*lp));

	if ((arg_count(cmd, snapshot_ARG) || arg_count(cmd, merge_ARG)) &&
	    (arg_count(cmd, mirrorlog_ARG) || arg_count(cmd, mirrors_ARG) ||
	     arg_count(cmd, repair_ARG))) {
		log_error("--snapshot or --merge argument cannot be mixed "
			  "with --mirrors, --repair or --log");
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
		if (!arg_count(cmd, name_ARG)) {
			log_error("Please name the new logical volume using '--name'");
			return 0;
		}

		lp->lv_split_name = arg_value(cmd, name_ARG);
		if (!apply_lvname_restrictions(lp->lv_split_name))
			return_0;

		lp->keep_mimages = 1;
		if (arg_sign_value(cmd, mirrors_ARG, 0) == SIGN_MINUS) {
			log_error("Argument to --splitmirrors"
				  " cannot be negative");
			return 0;
		}
		lp->mirrors = arg_uint_value(cmd, splitmirrors_ARG, 0);
		lp->mirrors_sign = SIGN_MINUS;
	} else if (arg_count(cmd, name_ARG)) {
		log_error("The 'name' argument is only valid"
			  " with --splitmirrors");
		return 0;
	}

	if (arg_count(cmd, merge_ARG))
		lp->merge = 1;

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

	if (lp->merge) {
		if (arg_count(cmd, regionsize_ARG) || arg_count(cmd, chunksize_ARG) ||
		    arg_count(cmd, zero_ARG) || arg_count(cmd, regionsize_ARG)) {
			log_error("Only --background and --interval are valid "
				  "arguments for snapshot merge");
			return 0;
		}

		if (!(lp->segtype = get_segtype_from_string(cmd, "snapshot")))
			return_0;

	} else if (lp->snapshot) {
		if (arg_count(cmd, regionsize_ARG)) {
			log_error("--regionsize is only available with mirrors");
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

	} else {	/* Mirrors */
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

		if (!(lp->segtype = get_segtype_from_string(cmd, "mirror")))
			return_0;
	}

	if (activation() && lp->segtype->ops->target_present &&
	    !lp->segtype->ops->target_present(cmd, NULL, NULL)) {
		log_error("%s: Required device-mapper target(s) not "
			  "detected in your kernel", lp->segtype->name);
		return 0;
	}

	if (!_lvconvert_name_params(lp, cmd, &argc, &argv))
		return_0;

	lp->pv_count = argc;
	lp->pvs = argv;
	lp->failed_pvs = NULL;

	return 1;
}

static struct volume_group *_get_lvconvert_vg(struct cmd_context *cmd,
					      const char *lv_name, const char *uuid)
{
	dev_close_all();

        return vg_read_for_update(cmd, extract_vgname(cmd, lv_name),
				  NULL, 0);
}

static struct logical_volume *_get_lvconvert_lv(struct cmd_context *cmd __attribute((unused)),
						struct volume_group *vg,
						const char *name,
						const char *uuid,
						uint32_t lv_type __attribute((unused)))
{
	struct logical_volume *lv = find_lv(vg, name);

	if (!lv || (uuid && strcmp(uuid, (char *)&lv->lvid)))
		return NULL;

	return lv;
}

static int _finish_lvconvert_mirror(struct cmd_context *cmd,
				    struct volume_group *vg,
				    struct logical_volume *lv,
				    struct dm_list *lvs_changed __attribute((unused)))
{
	int r = 0;

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
				   struct dm_list *lvs_changed __attribute((unused)))
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
				       const char *name __attribute((unused)),
				       struct daemon_parms *parms)
{
	float percent = 0.0;
	percent_range_t percent_range;

	if (!lv_snapshot_percent(lv, &percent, &percent_range)) {
		log_error("%s: Failed query for merging percentage. Aborting merge.", lv->name);
		return PROGRESS_CHECK_FAILED;
	} else if (percent_range == PERCENT_INVALID) {
		log_error("%s: Merging snapshot invalidated. Aborting merge.", lv->name);
		return PROGRESS_CHECK_FAILED;
	}

	if (parms->progress_display)
		log_print("%s: %s: %.1f%%", lv->name, parms->progress_title, percent);
	else
		log_verbose("%s: %s: %.1f%%", lv->name, parms->progress_title, percent);

	if (percent_range == PERCENT_0)
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
	int len = strlen(lv->vg->name) + strlen(lv->name) + 2;
	char *uuid = alloca(sizeof(lv->lvid));
	char *lv_full_name = alloca(len);


	if (!uuid || !lv_full_name)
		return_0;

	if (!dm_snprintf(lv_full_name, len, "%s/%s", lv->vg->name, lv->name))
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

static int _area_missing(struct lv_segment *lvseg, int s)
{
	if (seg_type(lvseg, s) == AREA_LV) {
		if (seg_lv(lvseg, s)->status & PARTIAL_LV)
			return 1;
	} else if ((seg_type(lvseg, s) == AREA_PV) &&
		   (seg_pv(lvseg, s)->status & MISSING_PV))
		return 1;

	return 0;
}

/* FIXME we want to handle mirror stacks here... */
static int _failed_mirrors_count(struct logical_volume *lv)
{
	struct lv_segment *lvseg;
	int ret = 0;
	int s;

	dm_list_iterate_items(lvseg, &lv->segments) {
		if (!seg_is_mirrored(lvseg))
			return -1;
		for (s = 0; s < lvseg->area_count; s++)
			if (_area_missing(lvseg, s))
				ret++;
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
		if (!(pvl->pv->status & MISSING_PV))
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

static int _using_corelog(struct logical_volume *lv)
{
	return !first_seg(_original_lv(lv))->log_lv;
}

static int _lv_update_log_type(struct cmd_context *cmd,
			       struct lvconvert_params *lp,
			       struct logical_volume *lv,
			       int log_count)
{
	struct logical_volume *original_lv = _original_lv(lv);
	if (_using_corelog(lv) && log_count) {
		if (!add_mirror_log(cmd, original_lv, log_count,
				    adjusted_mirror_region_size(
					lv->vg->extent_size,
					lv->le_count,
					lp->region_size),
				    lp->pvh, lp->alloc))
			return_0;
	} else if (!_using_corelog(lv) && !log_count) {
		if (!remove_mirror_log(cmd, original_lv,
				       lp->pv_count ? lp->pvh : NULL))
			return_0;
	}
	return 1;
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
			    !(pvl_vg->pv->status & MISSING_PV) ||
			    pvl_vg->pv->pe_alloc_count != 0)
				continue;

			/* FIXME: duplication of vgreduce code, move this to library */
			vg->free_count -= pvl_vg->pv->pe_count;
			vg->extent_count -= pvl_vg->pv->pe_count;
			vg->pv_count--;
			dm_list_del(&pvl_vg->list);

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

static int _lvconvert_mirrors(struct cmd_context *cmd, struct logical_volume *lv,
			      struct lvconvert_params *lp)
{
	struct lv_segment *seg;
	uint32_t existing_mirrors;
	const char *mirrorlog;
	unsigned log_count = 1;
	int r = 0;
	struct logical_volume *log_lv, *layer_lv;
	int failed_mirrors = 0, failed_log = 0;
	struct dm_list *old_pvh = NULL, *remove_pvs = NULL, *failed_pvs = NULL;

	int repair = arg_count(cmd, repair_ARG);
	int replace_log = 1, replace_mirrors = 1;
	int failure_code = 0;

	seg = first_seg(lv);
	existing_mirrors = lv_mirror_count(lv);

	/* If called with no argument, try collapsing the resync layers */
	if (!arg_count(cmd, mirrors_ARG) && !arg_count(cmd, mirrorlog_ARG) &&
	    !arg_count(cmd, corelog_ARG) && !arg_count(cmd, regionsize_ARG) &&
	    !arg_count(cmd, splitmirrors_ARG) && !repair) {
		if (find_temporary_mirror(lv) || (lv->status & CONVERTING))
			lp->need_polling = 1;
		return 1;
	}

	if (arg_count(cmd, mirrors_ARG) && repair) {
		log_error("You may only use one of --mirrors and --repair.");
		return 0;
	}

	/*
	 * Adjust required number of mirrors
	 *
	 * We check mirrors_ARG again to see if it
	 * was supplied.  If not, they want the mirror
	 * count to remain the same.  They may be changing
	 * the logging type.
	 */
	if (!arg_count(cmd, mirrors_ARG) && !arg_count(cmd, splitmirrors_ARG))
		lp->mirrors = existing_mirrors;
	else if (lp->mirrors_sign == SIGN_PLUS)
		lp->mirrors = existing_mirrors + lp->mirrors;
	else if (lp->mirrors_sign == SIGN_MINUS)
		lp->mirrors = existing_mirrors - lp->mirrors;
	else
		lp->mirrors += 1;

	if (lp->mirrors > DEFAULT_MIRROR_MAX_IMAGES) {
		log_error("Only up to %d images in mirror supported currently.",
			  DEFAULT_MIRROR_MAX_IMAGES);
		return 0;
	}

	/*
	 * If we are converting from one type of mirror to another, and
	 * the type of log wasn't specified, then let's keep the log type
	 * the same.
	 */
	if ((existing_mirrors > 1) && (lp->mirrors > 1) &&
	    (lp->mirrors != existing_mirrors) && !(lv->status & CONVERTING) &&
	    !arg_count(cmd, mirrorlog_ARG) && !arg_count(cmd, corelog_ARG)) {
		log_count = (first_seg(lv)->log_lv) ?
			lv_mirror_count(first_seg(lv)->log_lv) : 0;
	}

	if (repair) {
		cmd->handles_missing_pvs = 1;
		cmd->partial_activation = 1;
		lp->need_polling = 0;
		if (!(lv->status & PARTIAL_LV)) {
			log_error("The mirror is consistent. Nothing to repair.");
			return 1;
		}
		if ((failed_mirrors = _failed_mirrors_count(lv)) < 0)
			return_0;
		lp->mirrors -= failed_mirrors;
		log_error("Mirror status: %d of %d images failed.",
			  failed_mirrors, existing_mirrors);
		old_pvh = lp->pvh;
		if (!(failed_pvs = _failed_pv_list(lv->vg)))
			return_0;
		lp->pvh = lp->failed_pvs = failed_pvs;
		log_lv = first_seg(lv)->log_lv;
		if (!log_lv || log_lv->status & PARTIAL_LV) {
			failed_log = 1;
			log_count = 0;
		}
	} else {
		/*
		 * Did the user try to subtract more legs than available?
		 */
		if (lp->mirrors < 1) {
			log_error("Logical volume %s only has %" PRIu32 " mirrors.",
				  lv->name, existing_mirrors);
			return 0;
		}

		/*
		 * Adjust log type
		 */
		if (arg_count(cmd, corelog_ARG))
			log_count = 0;

		mirrorlog = arg_str_value(cmd, mirrorlog_ARG,
					  !log_count ? "core" : DEFAULT_MIRRORLOG);

		if (strcmp("core", mirrorlog) && !log_count) {
			log_error("--mirrorlog disk and --corelog "
				  "are incompatible");
			return 0;
		}

		if (!strcmp("disk", mirrorlog))
			log_count = 1;
		else if (!strcmp("core", mirrorlog))
			log_count = 0;
		else {
			log_error("Unknown mirrorlog type: %s", mirrorlog);
			return 0;
		}

		log_verbose("Setting logging type to %s", mirrorlog);
	}

	/*
	 * Region size must not change on existing mirrors
	 */
	if (arg_count(cmd, regionsize_ARG) && (lv->status & MIRRORED) &&
	    (lp->region_size != seg->region_size)) {
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

	if (repair)
		_lvconvert_mirrors_repair_ask(cmd, failed_log, failed_mirrors,
					      &replace_log, &replace_mirrors);

 restart:
	/*
	 * Converting from mirror to linear
	 */
	if ((lp->mirrors == 1)) {
		if (!(lv->status & MIRRORED)) {
			log_error("Logical volume %s is already not mirrored.",
				  lv->name);
			return 1;
		}
	}

	/*
	 * Downconversion.
	 */
	if (lp->mirrors < existing_mirrors) {
		/* Reduce number of mirrors */
		if (repair || lp->pv_count)
			remove_pvs = lp->pvh;

		if (lp->keep_mimages) {
			if (!lv_split_mirror_images(lv, lp->lv_split_name,
						    existing_mirrors - lp->mirrors,
						    remove_pvs))
				return 0;
		} else if (!lv_remove_mirrors(cmd, lv, existing_mirrors - lp->mirrors,
					      (!log_count || lp->mirrors == 1) ? 1U : 0U,
					      remove_pvs, 0))
			return_0;

		if (lp->mirrors > 1 &&
		    !_lv_update_log_type(cmd, lp, lv, log_count))
			return_0;
	} else if (!(lv->status & MIRRORED)) {
		/*
		 * Converting from linear to mirror
		 */

		/* FIXME Share code with lvcreate */

		/* FIXME Why is this restriction here?  Fix it! */
		dm_list_iterate_items(seg, &lv->segments) {
			if (seg_is_striped(seg) && seg->area_count > 1) {
				log_error("Mirrors of striped volumes are not yet supported.");
				return 0;
			}
		}

		/*
		 * FIXME should we give not only lp->pvh, but also all PVs
		 * currently taken by the mirror? Would make more sense from
		 * user perspective.
		 */
		if (!lv_add_mirrors(cmd, lv, lp->mirrors - 1, 1,
				    adjusted_mirror_region_size(
						lv->vg->extent_size,
						lv->le_count,
						lp->region_size),
				    log_count, lp->pvh, lp->alloc,
				    MIRROR_BY_LV)) {
			stack;
			return failure_code;
		}
		if (lp->wait_completion)
			lp->need_polling = 1;
	} else if (lp->mirrors > existing_mirrors || failed_mirrors) {
		if (lv->status & MIRROR_NOTSYNCED) {
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
			return failure_code;
		}

		/*
		 * Log addition/removal should be done before the layer
		 * insertion to make the end result consistent with
		 * linear-to-mirror conversion.
		 */
		if (!_lv_update_log_type(cmd, lp, lv, log_count)) {
			stack;
			return failure_code;
		}
		/* Insert a temporary layer for syncing,
		 * only if the original lv is using disk log. */
		if (seg->log_lv && !_insert_lvconvert_layer(cmd, lv)) {
			log_error("Failed to insert resync layer");
			return 0;
		}
		/* FIXME: can't have multiple mlogs. force corelog. */
		if (!lv_add_mirrors(cmd, lv, lp->mirrors - existing_mirrors, 1,
				    adjusted_mirror_region_size(
						lv->vg->extent_size,
						lv->le_count,
						lp->region_size),
				    0U, lp->pvh, lp->alloc,
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
			return failure_code;
		}
		lv->status |= CONVERTING;
		lp->need_polling = 1;
	}

	if (lp->mirrors == existing_mirrors) {
		if (_using_corelog(lv) != !log_count) {
			if (!_lv_update_log_type(cmd, lp, lv, log_count)) {
				stack;
				return failure_code;
			}
		} else {
			log_error("Logical volume %s already has %"
				  PRIu32 " mirror(s).", lv->name,
				  lp->mirrors - 1);
			if (lv->status & CONVERTING)
				lp->need_polling = 1;
			return 1;
		}
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	if (!vg_write(lv->vg))
		return_0;

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		goto out;
	}

	if (!vg_commit(lv->vg)) {
		if (!resume_lv(cmd, lv))
			stack;
		goto_out;
	}

	log_very_verbose("Updating \"%s\" in kernel", lv->name);

	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		goto out;
	}

	if (failed_log || failed_mirrors) {
		lp->pvh = old_pvh;
		if (failed_log && replace_log) {
			failed_log = 0;
			log_count = 1;
		}
		if (replace_mirrors)
			lp->mirrors += failed_mirrors;
		failed_mirrors = 0;
		existing_mirrors = lv_mirror_count(lv);
		/*
		 * Ignore failure in upconversion if this is a policy-driven
		 * action. If we got this far, only unexpected failures are
		 * reported.
		 */
		if (arg_count(cmd, use_policies_ARG))
			failure_code = 1;
		/* Now replace missing devices. */
		if (replace_log || replace_mirrors)
			goto restart;
	}

	if (!lp->need_polling)
		log_print("Logical volume %s converted.", lv->name);

	r = 1;
out:
	backup(lv->vg);
	return r;
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
		log_error("Unable to create a snapshot of a %s LV.",
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
	if (lv_info(cmd, origin, &info, 1, 0)) {
		if (info.open_count) {
			log_error("Can't merge over open origin volume");
			merge_on_activate = 1;
		}
	}
	if (lv_info(cmd, lv, &info, 1, 0)) {
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

	if (!deactivate_lv(cmd, lv)) {
		log_warn("WARNING: Unable to deactivate merging snapshot %s", lv->name);
		/* merge is running regardless of this deactivation failure */
	}

	lp->need_polling = 1;
	lp->lv_to_poll = origin;

	r = 1;
	log_print("Merging of volume %s started.", lv->name);
out:
	backup(lv->vg);
	return r;
}

static int lvconvert_single(struct cmd_context *cmd, struct logical_volume *lv,
			    void *handle)
{
	struct lvconvert_params *lp = handle;

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
		log_error("Can't repair non-mirrored LV \"%s\".", lv->name);
		return ECMD_FAILED;
	}

	if (lp->merge) {
		if (!lv_is_cow(lv)) {
			log_error("Logical volume \"%s\" is not a snapshot",
				  lv->name);
			return ECMD_FAILED;
		}
		if (!archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		if (!lvconvert_merge(cmd, lv, lp)) {
			stack;
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
	} else if (arg_count(cmd, mirrors_ARG) || (lv->status & MIRRORED)) {
		if (!archive(lv->vg)) {
			stack;
			return ECMD_FAILED;
		}
		if (!_lvconvert_mirrors(cmd, lv, lp)) {
			stack;
			return ECMD_FAILED;
		}

		/* If repairing and using policies, remove missing PVs from VG */
		if (arg_count(cmd, repair_ARG) && arg_count(cmd, use_policies_ARG))
			_remove_missing_empty_pv(lv->vg, lp->failed_pvs);
	}

	return ECMD_PROCESSED;
}

int lvconvert(struct cmd_context * cmd, int argc, char **argv)
{
	struct volume_group *vg;
	struct lv_list *lvl;
	struct lvconvert_params lp;
	int ret = ECMD_FAILED;
	struct lvinfo info;
	int saved_ignore_suspended_devices = ignore_suspended_devices();

	if (!_read_params(&lp, cmd, argc, argv)) {
		stack;
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, repair_ARG)) {
		init_ignore_suspended_devices(1);
		cmd->handles_missing_pvs = 1;
	}

	log_verbose("Checking for existing volume group \"%s\"", lp.vg_name);

	vg = vg_read_for_update(cmd, lp.vg_name, NULL, 0);
	if (vg_read_error(vg))
		goto out;

	if (!(lvl = find_lv_in_vg(vg, lp.lv_name))) {
		log_error("Logical volume \"%s\" not found in "
			  "volume group \"%s\"", lp.lv_name, lp.vg_name);
		goto bad;
	}

	if (lp.pv_count) {
		if (!(lp.pvh = create_pv_list(cmd->mem, vg, lp.pv_count,
					      lp.pvs, 0)))
			goto_bad;
	} else
		lp.pvh = &vg->pvs;

	lp.lv_to_poll = lvl->lv;
	ret = lvconvert_single(cmd, lvl->lv, &lp);

bad:
	unlock_vg(cmd, lp.vg_name);

	if (ret == ECMD_PROCESSED && lp.need_polling) {
		if (!lv_info(cmd, lp.lv_to_poll, &info, 1, 0) || !info.exists) {
			log_print("Conversion starts after activation");
			goto out;
		}
		ret = lvconvert_poll(cmd, lp.lv_to_poll,
				     lp.wait_completion ? 0 : 1U);
	}
out:
	init_ignore_suspended_devices(saved_ignore_suspended_devices);
	vg_release(vg);
	return ret;
}
