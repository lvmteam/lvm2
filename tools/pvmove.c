/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
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
#include "display.h"
#include "pvmove_poll.h"

#define PVMOVE_FIRST_TIME   0x00000001      /* Called for first time */

static int _pvmove_target_present(struct cmd_context *cmd, int clustered)
{
	const struct segment_type *segtype;
	unsigned attr = 0;
	int found = 1;
	static int _clustered_found = -1;

	if (clustered && _clustered_found >= 0)
		return _clustered_found;

	if (!(segtype = get_segtype_from_string(cmd, "mirror")))
		return_0;

	if (activation() && segtype->ops->target_present &&
	    !segtype->ops->target_present(cmd, NULL, clustered ? &attr : NULL))
		found = 0;

	if (activation() && clustered) {
		if (found && (attr & MIRROR_LOG_CLUSTERED))
			_clustered_found = found = 1;
		else
			_clustered_found = found = 0;
	}

	return found;
}

static unsigned _pvmove_is_exclusive(struct cmd_context *cmd,
				     struct volume_group *vg)
{
	if (vg_is_clustered(vg))
		if (!_pvmove_target_present(cmd, 1))
			return 1;

	return 0;
}

/* Allow /dev/vgname/lvname, vgname/lvname or lvname */
static const char *_extract_lvname(struct cmd_context *cmd, const char *vgname,
				   const char *arg)
{
	const char *lvname;

	/* Is an lvname supplied directly? */
	if (!strchr(arg, '/'))
		return arg;

	lvname = skip_dev_dir(cmd, arg, NULL);
	while (*lvname == '/')
		lvname++;
	if (!strchr(lvname, '/')) {
		log_error("--name takes a logical volume name");
		return NULL;
	}
	if (strncmp(vgname, lvname, strlen(vgname)) ||
	    (lvname += strlen(vgname), *lvname != '/')) {
		log_error("Named LV and old PV must be in the same VG");
		return NULL;
	}
	while (*lvname == '/')
		lvname++;
	if (!*lvname) {
		log_error("Incomplete LV name supplied with --name");
		return NULL;
	}
	return lvname;
}

/* Create list of PVs for allocation of replacement extents */
static struct dm_list *_get_allocatable_pvs(struct cmd_context *cmd, int argc,
					 char **argv, struct volume_group *vg,
					 struct physical_volume *pv,
					 alloc_policy_t alloc)
{
	struct dm_list *allocatable_pvs, *pvht, *pvh;
	struct pv_list *pvl;

	if (argc)
		allocatable_pvs = create_pv_list(cmd->mem, vg, argc, argv, 1);
	else
		allocatable_pvs = clone_pv_list(cmd->mem, &vg->pvs);

	if (!allocatable_pvs)
		return_NULL;

	dm_list_iterate_safe(pvh, pvht, allocatable_pvs) {
		pvl = dm_list_item(pvh, struct pv_list);

		/* Don't allocate onto the PV we're clearing! */
		if ((alloc != ALLOC_ANYWHERE) && (pvl->pv->dev == pv_dev(pv))) {
			dm_list_del(&pvl->list);
			continue;
		}

		/* Remove PV if full */
		if (pvl->pv->pe_count == pvl->pv->pe_alloc_count)
			dm_list_del(&pvl->list);
	}

	if (dm_list_empty(allocatable_pvs)) {
		log_error("No extents available for allocation");
		return NULL;
	}

	return allocatable_pvs;
}

/*
 * _trim_allocatable_pvs
 * @alloc_list
 * @trim_list
 *
 * Remove PVs in 'trim_list' from 'alloc_list'.
 *
 * Returns: 1 on success, 0 on error
 */
static int _trim_allocatable_pvs(struct dm_list *alloc_list,
				 struct dm_list *trim_list,
				 alloc_policy_t alloc)
{
	struct dm_list *pvht, *pvh, *trim_pvh;
	struct pv_list *pvl, *trim_pvl;

	if (!alloc_list) {
		log_error(INTERNAL_ERROR "alloc_list is NULL");
		return 0;
	}

	if (!trim_list || dm_list_empty(trim_list))
		return 1; /* alloc_list stays the same */

	dm_list_iterate_safe(pvh, pvht, alloc_list) {
		pvl = dm_list_item(pvh, struct pv_list);

		dm_list_iterate(trim_pvh, trim_list) {
			trim_pvl = dm_list_item(trim_pvh, struct pv_list);

			/* Don't allocate onto a trim PV */
			if ((alloc != ALLOC_ANYWHERE) &&
			    (pvl->pv == trim_pvl->pv)) {
				dm_list_del(&pvl->list);
				break;  /* goto next in alloc_list */
			}
		}
	}
	return 1;
}

/*
 * Replace any LV segments on given PV with temporary mirror.
 * Returns list of LVs changed.
 */
static int _insert_pvmove_mirrors(struct cmd_context *cmd,
				  struct logical_volume *lv_mirr,
				  struct dm_list *source_pvl,
				  struct logical_volume *lv,
				  struct dm_list *lvs_changed)

{
	struct pv_list *pvl;
	uint32_t prev_le_count;

	/* Only 1 PV may feature in source_pvl */
	pvl = dm_list_item(source_pvl->n, struct pv_list);

	prev_le_count = lv_mirr->le_count;
	if (!insert_layer_for_segments_on_pv(cmd, lv, lv_mirr, PVMOVE,
					     pvl, lvs_changed))
		return_0;

	/* check if layer was inserted */
	if (lv_mirr->le_count - prev_le_count) {
		lv->status |= LOCKED;

		log_verbose("Moving %u extents of logical volume %s/%s",
			    lv_mirr->le_count - prev_le_count,
			    lv->vg->name, lv->name);
	}

	return 1;
}

/*
 * Is 'lv' a sub_lv of the LV by the name of 'lv_name'?
 *
 * Returns: 1 if true, 0 otherwise
 */
static int sub_lv_of(struct logical_volume *lv, const char *lv_name)
{
	struct lv_segment *seg;

	/* Sub-LVs only ever have one segment using them */
	if (dm_list_size(&lv->segs_using_this_lv) != 1)
		return 0;

	if (!(seg = get_only_segment_using_this_lv(lv)))
		return_0;

	if (!strcmp(seg->lv->name, lv_name))
		return 1;

	/* Continue up the tree */
	return sub_lv_of(seg->lv, lv_name);
}

/*
 * parent_lv_is_cache_type
 *
 * FIXME: This function can be removed when 'pvmove' is supported for
 *        cache types.
 *
 * If this LV is below a cache LV (at any depth), return 1.
 */
static int parent_lv_is_cache_type(struct logical_volume *lv)
{
	struct lv_segment *seg;

	/* Sub-LVs only ever have one segment using them */
	if (dm_list_size(&lv->segs_using_this_lv) != 1)
		return 0;

	if (!(seg = get_only_segment_using_this_lv(lv)))
		return_0;

	if (lv_is_cache_type(seg->lv))
		return 1;

	/* Continue up the tree */
	return parent_lv_is_cache_type(seg->lv);
}

/* Create new LV with mirror segments for the required copies */
static struct logical_volume *_set_up_pvmove_lv(struct cmd_context *cmd,
						struct volume_group *vg,
						struct dm_list *source_pvl,
						const char *lv_name,
						struct dm_list *allocatable_pvs,
						alloc_policy_t alloc,
						struct dm_list **lvs_changed,
						unsigned *exclusive)
{
	struct logical_volume *lv_mirr, *lv;
	struct lv_segment *seg;
	struct lv_list *lvl;
	struct dm_list trim_list;
	uint32_t log_count = 0;
	int lv_found = 0;
	int lv_skipped = 0;
	int lv_active_count = 0;
	int lv_exclusive_count = 0;

	/* FIXME Cope with non-contiguous => splitting existing segments */
	if (!(lv_mirr = lv_create_empty("pvmove%d", NULL,
					LVM_READ | LVM_WRITE,
					ALLOC_CONTIGUOUS, vg))) {
		log_error("Creation of temporary pvmove LV failed");
		return NULL;
	}

	lv_mirr->status |= (PVMOVE | LOCKED);

	if (!(*lvs_changed = dm_pool_alloc(cmd->mem, sizeof(**lvs_changed)))) {
		log_error("lvs_changed list struct allocation failed");
		return NULL;
	}

	dm_list_init(*lvs_changed);

	/*
	 * First,
	 * use top-level RAID and mirror LVs to build a list of PVs
	 * that must be avoided during allocation.  This is necessary
	 * to maintain redundancy of those targets, but it is also
	 * sub-optimal.  Avoiding entire PVs in this way limits our
	 * ability to find space for other segment types.  In the
	 * majority of cases, however, this method will suffice and
	 * in the cases where it does not, the user can issue the
	 * pvmove on a per-LV basis.
	 *
	 * FIXME: Eliminating entire PVs places too many restrictions
	 *        on allocation.
	 */
	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if (lv == lv_mirr)
			continue;

		if (lv_name && strcmp(lv->name, lv_name))
			continue;

		/*
		 * RAID, thin and snapshot-related LVs are not
		 * processed in a cluster, so we don't have to
		 * worry about avoiding certain PVs in that context.
		 *
		 * Allow clustered mirror, but not raid mirror.
		 */
		if (vg_is_clustered(lv->vg) && !lv_is_mirror_type(lv))
			continue;

		if (!lv_is_on_pvs(lv, source_pvl))
			continue;

		if (lv_is_converting(lv) || lv_is_merging(lv)) {
			log_error("Unable to pvmove when %s volumes are present",
				  lv_is_converting(lv) ?
				  "converting" : "merging");
			return NULL;
		}

		if (seg_is_raid(first_seg(lv)) ||
		    seg_is_mirrored(first_seg(lv))) {
			dm_list_init(&trim_list);

			if (!get_pv_list_for_lv(lv->vg->cmd->mem,
						lv, &trim_list))
				return_NULL;

			if (!_trim_allocatable_pvs(allocatable_pvs,
						   &trim_list, alloc))
				return_NULL;
		}
	}

	/*
	 * Second,
	 * use bottom-level LVs (like *_mimage_*, *_mlog, *_rmeta_*, etc)
	 * to find segments to be moved and then set up mirrors.
	 */
	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if (lv == lv_mirr)
			continue;
		if (lv_name) {
			if (strcmp(lv->name, lv_name) && !sub_lv_of(lv, lv_name))
				continue;
			lv_found = 1;
		}

		if (!lv_is_on_pvs(lv, source_pvl))
			continue;

		if (lv_is_cache_type(lv)) {
			log_print_unless_silent("Skipping %s LV, %s",
						lv_is_cache(lv) ? "cache" :
						lv_is_cache_pool(lv) ?
						"cache-pool" : "cache-related",
						lv->name);
			lv_skipped = 1;
			continue;
		}

		if (parent_lv_is_cache_type(lv)) {
			log_print_unless_silent("Skipping %s because a parent"
						" is of cache type", lv->name);
			lv_skipped = 1;
			continue;
		}

		/*
		 * If the VG is clustered, we are unable to handle
		 * snapshots, origins, thin types, RAID or mirror
		 */
		if (vg_is_clustered(vg) &&
		    (lv_is_origin(lv) || lv_is_cow(lv) ||
		     lv_is_thin_type(lv) || lv_is_raid_type(lv))) {
			log_print_unless_silent("Skipping %s LV %s",
						lv_is_origin(lv) ? "origin" :
						lv_is_cow(lv) ?
						"snapshot-related" :
						lv_is_thin_volume(lv) ? "thin" :
						lv_is_thin_pool(lv) ?
						"thin-pool" :
						lv_is_thin_type(lv) ?
						"thin-related" :
						seg_is_raid(first_seg(lv)) ?
						"RAID" :
						lv_is_raid_type(lv) ?
						"RAID-related" : "",
						lv->name);
			lv_skipped = 1;
			continue;
		}

		seg = first_seg(lv);
		if (seg_is_raid(seg) || seg_is_mirrored(seg) ||
		    lv_is_thin_volume(lv) || lv_is_thin_pool(lv)) {
			/*
			 * Pass over top-level LVs - they were handled.
			 * Allow sub-LVs to proceed.
			 */
			continue;
		}

		if (lv_is_locked(lv)) {
			lv_skipped = 1;
			log_print_unless_silent("Skipping locked LV %s", lv->name);
			continue;
		}

		if (vg_is_clustered(vg) &&
		    lv_is_active_exclusive_remotely(lv)) {
			lv_skipped = 1;
			log_print_unless_silent("Skipping LV %s which is activated "
						"exclusively on remote node.", lv->name);
			continue;
		}

		if (vg_is_clustered(vg)) {
			if (lv_is_active_exclusive_locally(lv))
				lv_exclusive_count++;
			else if (lv_is_active(lv))
				lv_active_count++;
		}

		if (!_insert_pvmove_mirrors(cmd, lv_mirr, source_pvl, lv,
					    *lvs_changed))
			return_NULL;
	}

	if (lv_name && !lv_found) {
		log_error("Logical volume %s not found.", lv_name);
		return NULL;
	}

	/* Is temporary mirror empty? */
	if (!lv_mirr->le_count) {
		if (lv_skipped)
			log_error("All data on source PV skipped. "
				  "It contains locked, hidden or "
				  "non-top level LVs only.");
		log_error("No data to move for %s", vg->name);
		return NULL;
	}

	if (vg_is_clustered(vg) && lv_active_count && *exclusive) {
		log_error("Cannot move in clustered VG %s, "
			  "clustered mirror (cmirror) not detected "
			  "and LVs are activated non-exclusively.",
			  vg->name);
		return NULL;
	}

	if (vg_is_clustered(vg) && lv_exclusive_count) {
		if (lv_active_count) {
			log_error("Cannot move in clustered VG %s "
				  "if some LVs are activated "
				  "exclusively while others don't.",
				  vg->name);
			return NULL;
		}
		*exclusive = 1;
	}

	if (!lv_add_mirrors(cmd, lv_mirr, 1, 1, 0, 0, log_count,
			    allocatable_pvs, alloc,
			    (arg_count(cmd, atomic_ARG)) ?
			    MIRROR_BY_SEGMENTED_LV : MIRROR_BY_SEG)) {
		log_error("Failed to convert pvmove LV to mirrored");
		return_NULL;
	}

	if (!split_parent_segments_for_layer(cmd, lv_mirr)) {
		log_error("Failed to split segments being moved");
		return_NULL;
	}

	return lv_mirr;
}

static int _activate_lv(struct cmd_context *cmd, struct logical_volume *lv_mirr,
			unsigned exclusive)
{
	int r = 0;

	if (exclusive || lv_is_active_exclusive(lv_mirr))
		r = activate_lv_excl(cmd, lv_mirr);
	else
		r = activate_lv(cmd, lv_mirr);

	if (!r)
		stack;

	return r;
}

/*
 * Called to set up initial pvmove LV only.
 * (Not called after first or any other section completes.)
 */
static int _update_metadata(struct cmd_context *cmd, struct volume_group *vg,
			    struct logical_volume *lv_mirr,
			    struct dm_list *lvs_changed, unsigned exclusive)
{
	int r = 0;

	log_verbose("Setting up pvmove in on-disk volume group metadata.");
	if (!vg_write(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		return 0;
	}

	if (!suspend_lvs(cmd, lvs_changed, vg)) {
		log_error("ABORTING: Temporary pvmove mirror activation failed.");
		/* FIXME Add a recovery path for first time too. */
		return 0;
	}

	/* Commit on-disk metadata */
	if (!vg_commit(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		if (!resume_lvs(cmd, lvs_changed))
			log_error("Unable to resume logical volumes.");
		return 0;
	}

	/* Activate the temporary mirror LV */
	/* Only the first mirror segment gets activated as a mirror */
	/* FIXME: Add option to use a log */
	if (!exclusive && _pvmove_is_exclusive(cmd, vg))
		exclusive = 1;

	if (!_activate_lv(cmd, lv_mirr, exclusive)) {
		if (test_mode()) {
			r = 1;
			goto out;
		}

		/*
		 * FIXME Run --abort internally here.
		 */
		log_error("ABORTING: Temporary pvmove mirror activation failed. Run pvmove --abort.");
		goto out;
	}

	r = 1;

out:
	if (!resume_lvs(cmd, lvs_changed)) {
		log_error("Unable to resume logical volumes.");
		r = 0;
	}

	if (r)
		backup(vg);

	return r;
}

static int _copy_id_components(struct cmd_context *cmd,
			       const struct logical_volume *lv, char **vg_name,
			       char **lv_name, union lvid *lvid)
{
	if (!(*vg_name = dm_pool_strdup(cmd->mem, lv->vg->name)) ||
	    !(*lv_name = dm_pool_strdup(cmd->mem, lv->name))) {
		log_error("Failed to clone VG or LV name.");
		return 0;
	}

	*lvid = lv->lvid;

	return 1;
}

static int _set_up_pvmove(struct cmd_context *cmd, const char *pv_name,
			  int argc, char **argv, union lvid *lvid, char **vg_name_copy,
			  char **lv_mirr_name)
{
	const char *lv_name = NULL;
	const char *vg_name;
	char *pv_name_arg;
	struct volume_group *vg;
	struct dm_list *source_pvl;
	struct dm_list *allocatable_pvs;
	alloc_policy_t alloc;
	struct dm_list *lvs_changed;
	struct physical_volume *pv;
	struct logical_volume *lv_mirr;
	unsigned flags = PVMOVE_FIRST_TIME;
	unsigned exclusive;
	int r = ECMD_FAILED;

	pv_name_arg = argv[0];
	argc--;
	argv++;

	/* Find PV (in VG) */
	if (!(pv = find_pv_by_name(cmd, pv_name, 0, 0))) {
		stack;
		return EINVALID_CMD_LINE;
	}

	vg_name = pv_vg_name(pv);

	if (arg_count(cmd, name_ARG)) {
		if (!(lv_name = _extract_lvname(cmd, vg_name, arg_value(cmd, name_ARG)))) {
			stack;
			free_pv_fid(pv);
			return EINVALID_CMD_LINE;
		}

		if (!validate_name(lv_name)) {
			log_error("Logical volume name %s is invalid", lv_name);
			free_pv_fid(pv);
			return EINVALID_CMD_LINE;
		}
	}

	/* Read VG */
	log_verbose("Finding volume group \"%s\"", vg_name);

	vg = vg_read(cmd, vg_name, NULL, READ_FOR_UPDATE);
	if (vg_read_error(vg)) {
		release_vg(vg);
		return_ECMD_FAILED;
	}

	exclusive = _pvmove_is_exclusive(cmd, vg);

	if ((lv_mirr = find_pvmove_lv(vg, pv_dev(pv), PVMOVE))) {
		log_print_unless_silent("Detected pvmove in progress for %s", pv_name);
		if (argc || lv_name)
			log_error("Ignoring remaining command line arguments");

		if (!(lvs_changed = lvs_using_lv(cmd, vg, lv_mirr))) {
			log_error("ABORTING: Failed to generate list of moving LVs");
			goto out;
		}

		/* Ensure mirror LV is active */
		if (!_activate_lv(cmd, lv_mirr, exclusive)) {
			log_error("ABORTING: Temporary mirror activation failed.");
			goto out;
		}

		flags &= ~PVMOVE_FIRST_TIME;
	} else {
		/* Determine PE ranges to be moved */
		if (!(source_pvl = create_pv_list(cmd->mem, vg, 1,
						  &pv_name_arg, 0)))
			goto_out;

		alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, ALLOC_INHERIT);
		if (alloc == ALLOC_INHERIT)
			alloc = vg->alloc;

		/* Get PVs we can use for allocation */
		if (!(allocatable_pvs = _get_allocatable_pvs(cmd, argc, argv,
							     vg, pv, alloc)))
			goto_out;

		if (!archive(vg))
			goto_out;

		if (!(lv_mirr = _set_up_pvmove_lv(cmd, vg, source_pvl, lv_name,
						  allocatable_pvs, alloc,
						  &lvs_changed, &exclusive)))
			goto_out;
	}

	/* Lock lvs_changed and activate (with old metadata) */
	if (!activate_lvs(cmd, lvs_changed, exclusive))
		goto_out;

	/* FIXME Presence of a mirror once set PVMOVE - now remove associated logic */
	/* init_pvmove(1); */
	/* vg->status |= PVMOVE; */

	if (!_copy_id_components(cmd, lv_mirr, vg_name_copy, lv_mirr_name, lvid))
		goto out;

	if (flags & PVMOVE_FIRST_TIME)
		if (!_update_metadata(cmd, vg, lv_mirr, lvs_changed, exclusive))
			goto_out;

	/* LVs are all in status LOCKED */
	r = ECMD_PROCESSED;
out:
	free_pv_fid(pv);
	unlock_and_release_vg(cmd, vg, vg_name);
	return r;
}

static int _read_poll_id_from_pvname(struct cmd_context *cmd, const char *pv_name,
				     union lvid *lvid, char **vg_name_copy,
				     char **lv_name_copy, unsigned *in_progress)
{
	int ret = 0;
	const char *vg_name;
	struct logical_volume *lv;
	struct physical_volume *pv;
	struct volume_group *vg;

	if (!pv_name) {
		log_error(INTERNAL_ERROR "Invalid PV name parameter.");
		return 0;
	}

	if (!(pv = find_pv_by_name(cmd, pv_name, 0, 0)))
		return_0;

	vg_name = pv_vg_name(pv);

	/* need read-only access */
	vg = vg_read(cmd, vg_name, NULL, 0);
	if (vg_read_error(vg)) {
		log_error("ABORTING: Can't read VG for %s.", pv_name);
		release_vg(vg);
		free_pv_fid(pv);
		return 0;
	}

	if (!(lv = find_pvmove_lv(vg, pv_dev(pv), PVMOVE))) {
		log_print_unless_silent("%s: No pvmove in progress - already finished or aborted.",
					pv_name);
		ret = 1;
		*in_progress = 0;
	} else if (_copy_id_components(cmd, lv, vg_name_copy, lv_name_copy, lvid)) {
		ret = 1;
		*in_progress = 1;
	}

	unlock_and_release_vg(cmd, vg, vg_name);
	free_pv_fid(pv);
	return ret;
}

static struct poll_functions _pvmove_fns = {
	.get_copy_name_from_lv = get_pvmove_pvname_from_lv_mirr,
	.poll_progress = poll_mirror_progress,
	.update_metadata = pvmove_update_metadata,
	.finish_copy = pvmove_finish,
};

static void _destroy_id(struct cmd_context *cmd, struct poll_operation_id *id)
{
	if (!id)
		return;

	dm_pool_free(cmd->mem, id);
}

static struct poll_operation_id *_create_id(struct cmd_context *cmd,
					    const char *pv_name,
					    const char *vg_name,
					    const char *lv_name,
					    const char *uuid)
{
	struct poll_operation_id *id = dm_pool_alloc(cmd->mem, sizeof(struct poll_operation_id));
	if (!id) {
		log_error("Poll operation ID allocation failed.");
		return NULL;
	}

	id->vg_name = vg_name ? dm_pool_strdup(cmd->mem, vg_name) : NULL;
	id->lv_name = lv_name ? dm_pool_strdup(cmd->mem, lv_name) : NULL;
	id->display_name = pv_name ? dm_pool_strdup(cmd->mem, pv_name) : NULL;
	id->uuid = uuid ? dm_pool_strdup(cmd->mem, uuid) : NULL;

	if (!id->vg_name || !id->lv_name || !id->display_name || !id->uuid) {
		log_error("Failed to copy one or more poll operation ID members.");
		_destroy_id(cmd, id);
		id = NULL;
	}

	return id;
}

int pvmove_poll(struct cmd_context *cmd, const char *pv_name,
		const char *uuid, const char *vg_name,
		const char *lv_name, unsigned background)
{
	int r;
	struct poll_operation_id *id = NULL;

	if (test_mode())
		return ECMD_PROCESSED;

	if (uuid) {
		id = _create_id(cmd, pv_name, vg_name, lv_name, uuid);
		if (!id) {
			log_error("Failed to allocate poll identifier for pvmove.");
			return ECMD_FAILED;
		}
	}

	r = poll_daemon(cmd, background, PVMOVE, &_pvmove_fns, "Moved", id);

	_destroy_id(cmd, id);

	return r;
}

int pvmove(struct cmd_context *cmd, int argc, char **argv)
{
	char *colon;
	int ret;
	unsigned in_progress = 1;
	union lvid *lvid = NULL;
	char *pv_name = NULL, *vg_name = NULL, *lv_name = NULL;

	/* dm raid1 target must be present in every case */
	if (!_pvmove_target_present(cmd, 0)) {
		log_error("Required device-mapper target(s) not "
			  "detected in your kernel");
		return ECMD_FAILED;
	}

	if (argc) {
		if (!(lvid = dm_pool_alloc(cmd->mem, sizeof(*lvid)))) {
			log_error("Failed to allocate lvid.");
			return ECMD_FAILED;
		}

		if (!(pv_name = dm_pool_strdup(cmd->mem, argv[0]))) {
			log_error("Failed to clone PV name.");
			return ECMD_FAILED;
		}

		dm_unescape_colons_and_at_signs(pv_name, &colon, NULL);

		/* Drop any PE lists from PV name */
		if (colon)
			*colon = '\0';

		if (!arg_count(cmd, abort_ARG)) {
			if ((ret = _set_up_pvmove(cmd, pv_name, argc, argv, lvid, &vg_name, &lv_name)) != ECMD_PROCESSED) {
				stack;
				return ret;
			}
		} else {
			if (!_read_poll_id_from_pvname(cmd, pv_name, lvid, &vg_name, &lv_name, &in_progress))
				return_ECMD_FAILED;

			if (!in_progress)
				return ECMD_PROCESSED;
		}
	}

	return pvmove_poll(cmd, pv_name, lvid ? lvid->s : NULL, vg_name, lv_name,
			   arg_is_set(cmd, background_ARG));
}
