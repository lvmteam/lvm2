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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"

#include "lib/lvmpolld/polldaemon.h"
#include "lib/display/display.h"
#include "pvmove_poll.h"
#include "lib/lvmpolld/lvmpolld-client.h"

struct pvmove_params {
	char *pv_name_arg; /* original unmodified arg */
	char *lv_name_arg; /* original unmodified arg */
	alloc_policy_t alloc;
	int pv_count;
	char **pv_names;

	union lvid *lvid;
	char *id_vg_name;
	char *id_lv_name;
	unsigned in_progress;
	int setup_result;
	int found_pv;
};

static int _pvmove_target_present(struct cmd_context *cmd)
{
	const struct segment_type *segtype;

	if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_MIRROR)))
		return_0;

	if (activation() && segtype->ops->target_present &&
	    !segtype->ops->target_present(cmd, NULL, NULL))
		return 0;

	return 1;
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
		log_error("--name takes a logical volume name.");
		return NULL;
	}
	if (strncmp(vgname, lvname, strlen(vgname)) ||
	    (lvname += strlen(vgname), *lvname != '/')) {
		log_error("Named LV and old PV must be in the same VG.");
		return NULL;
	}
	while (*lvname == '/')
		lvname++;
	if (!*lvname) {
		log_error("Incomplete LV name supplied with --name.");
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
		log_error("No extents available for allocation.");
		return NULL;
	}

	return allocatable_pvs;
}

/*
 * Replace any LV segments on given PV with temporary mirror.
 * Affected LVs are added to lvs_changed.
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

		log_verbose("Moving %u extents of logical volume %s.",
			    lv_mirr->le_count - prev_le_count,
			    display_lvname(lv));
	}

	return 1;
}

/*
 * Is 'lv' a sub_lv of the LV by the name of 'lv_name'?
 *
 * Returns: 1 if true, 0 otherwise
 */
static int _sub_lv_of(struct logical_volume *lv, const char *lv_name)
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
	return _sub_lv_of(seg->lv, lv_name);
}

/* Return 0 if the top-level LV cannot be pvmoved. source_pvl may be NULL. */
static int _lv_is_pvmoveable(const struct logical_volume *lv,
			     struct dm_list *source_pvl)
{
	struct logical_volume *lv_cachevol;
	struct logical_volume *lv_orig;

	if (lv_is_lockd_sanlock_lv(lv)) {
		log_error("Unable to pvmove internal sanlock LV.");
		return 0;
	}

	if (lv_is_locked(lv)) {
		log_error("LV %s is already locked by another pvmove.",
			  display_lvname(lv));
		return 0;
	}

	/*
	 * FIXME: CONVERTING and MERGING are transient in-memory flags
	 * (not stored in on-disk metadata), so a separate pvmove command
	 * will never see them.  Unreachable until these flags are
	 * persisted or pvmove is called from within lvconvert.
	 */
	if (lv_is_converting(lv) || lv_is_merging(lv)) {
		log_error("Unable to pvmove when %s volume %s is present.",
			  lv_is_converting(lv) ? "converting" : "merging",
			  display_lvname(lv));
		return 0;
	}

	if (lv_is_writecache_cachevol(lv)) {
		log_error("Unable to pvmove device used for writecache.");
		return 0;
	}

	if (lv_is_writecache(lv)) {
		lv_cachevol = first_seg(lv)->writecache;
		if (source_pvl && lv_is_on_pvs(lv_cachevol, source_pvl)) {
			log_error("Unable to move device used for writecache cachevol %s.",
				  display_lvname(lv_cachevol));
			return 0;
		}
	}

	if (lv_is_raid(lv) && lv_raid_has_integrity(lv)) {
		log_error("Unable to pvmove device used for raid with integrity.");
		return 0;
	}

	if (lv_is_cache(lv) || lv_is_writecache(lv)) {
		lv_orig = seg_lv(first_seg(lv), 0);
		if (lv_is_raid(lv_orig) && lv_raid_has_integrity(lv_orig)) {
			log_error("Unable to pvmove raid LV with integrity under cache.");
			return 0;
		}
	}

	return 1;
}

/*
 * Finalize the pvmove mirror LV: check that it has segments,
 * convert to mirrored target and split parent segments.
 *
 * RAID/mirror redundancy during pvmove:
 *
 * When moving RAID sub-LVs, the mirror allocation must avoid placing
 * the new copy on a PV that already holds a sibling sub-LV of the
 * same RAID array (otherwise redundancy is broken).
 *
 * This is handled per-segment inside the allocator via parallel_areas,
 * built by build_parallel_areas_from_lv() in lv_manip.c.  Each pvmove
 * segment links back to its source sub-LV through pvmove_source_seg.
 * _add_raid_exclusion_pvs() uses that link to find the RAID parent,
 * then adds PVs of all sibling sub-LVs to the segment's avoidance
 * list -- except the same-index partner (rmeta<->rimage collocation).
 *
 * The call path is:
 *   lv_add_mirrors(MIRROR_BY_SEG)
 *     -> _add_mirrors_that_preserve_segments()  [mirror.c]
 *       -> build_parallel_areas_from_lv(lv_mirr, use_pvmove_parent=1)
 *            per segment: _add_raid_exclusion_pvs()  [lv_manip.c]
 *       -> allocate_extents(parallel_areas)
 *
 * Because parallel_areas is per-segment, each RAID LV gets its own
 * exclusion constraint -- unlike global PV exclusion which would
 * remove PVs from allocatable_pvs for ALL LVs at once, potentially
 * blocking moves that are individually valid.
 */
static int _finalize_pvmove_lv(struct cmd_context *cmd,
			       struct logical_volume *lv_mirr,
			       struct dm_list *allocatable_pvs,
			       alloc_policy_t alloc)
{
	uint32_t log_count = 0;

	/* Is temporary mirror empty? */
	if (!lv_mirr->le_count) {
		log_error("No data to move for %s.", lv_mirr->vg->name);
		return 0;
	}

	if (!lv_add_mirrors(cmd, lv_mirr, 1, 1, 0,
			    get_default_region_size(cmd),
			    log_count, allocatable_pvs, alloc,
			    (arg_is_set(cmd, atomic_ARG)) ?
			    MIRROR_BY_SEGMENTED_LV : MIRROR_BY_SEG)) {
		log_error("Failed to convert pvmove LV to mirrored.");
		return 0;
	}

	if (!split_parent_segments_for_layer(cmd, lv_mirr)) {
		log_error("Failed to split segments being moved.");
		return 0;
	}

	return 1;
}

/*
 * Find bottom-level striped LVs on source PVs.
 */
static int _find_moving_lvs(struct cmd_context *cmd,
			    struct volume_group *vg,
			    struct dm_list *source_pvl,
			    struct logical_volume *lv_move,
			    struct dm_list *moving_lvs)
{
	struct logical_volume *lv;
	struct lv_list *lvl, *new_lvl;
	int lv_found = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		/* Only striped sub-LVs of lv_move should be moved */
		if (lv_move) {
			if (strcmp(lv->name, lv_move->name) && !_sub_lv_of(lv, lv_move->name))
				continue;
			lv_found = 1;
		}

		if (!lv_is_striped(lv))
			continue;

		if (!lv_is_on_pvs(lv, source_pvl))
			continue;

		if (!(new_lvl = dm_pool_alloc(cmd->mem, sizeof(*new_lvl)))) {
			log_error("Failed to allocate LV list entry.");
			return 0;
		}
		new_lvl->lv = lv;
		dm_list_add(moving_lvs, &new_lvl->list);
	}

	if (lv_move && !lv_found) {
		log_error("Logical volume %s not found.", display_lvname(lv_move));
		return 0;
	}

	if (dm_list_empty(moving_lvs)) {
		log_error("No data to move for %s.", vg->name);
		return 0;
	}

	return 1;
}

/*
 * If lv_move is set (named pvmove), failure to move is a command error.
 * Otherwise, unmovable LVs are skipped with a warning.
 */
static int _skip_unmovable_lvs(const struct logical_volume *lv_move,
			       struct dm_list *source_pvl,
			       struct dm_list *moving_lvs)
{
	struct logical_volume *lv;
	const struct logical_volume *lv_top;
	struct lv_list *lvl, *lvl2;

	dm_list_iterate_items_safe(lvl, lvl2, moving_lvs) {
		lv = lvl->lv;

		if (lv_is_locked(lv)) {
			if (lv_move) {
				log_error("LV %s is already locked by another pvmove.",
					  display_lvname(lv_move));
				return 0;
			}
			log_warn("WARNING: Not moving LV %s: locked by another pvmove.",
				 display_lvname(lv));
			dm_list_del(&lvl->list);
			continue;
		}

		/* Note: "lock holder" terminology is outdated, it's really the top LV when "lv" is a sub LV */
		lv_top = lv_lock_holder(lv);

		if (!_lv_is_pvmoveable(lv_top, source_pvl)) {
			if (lv_move) {
				log_error("LV %s is not moveable.",
					  display_lvname(lv_move));
				return 0;
			}
			log_warn("WARNING: Not moving LV %s: not moveable.",
				 display_lvname(lv));
			dm_list_del(&lvl->list);
		}
	}
	return 1;
}

/*
 * Probe whether lv_top's cluster lock is held locally.
 * Active LVs: query the lock state without acquiring.
 * Inactive LVs: try to acquire EX, then immediately release.
 * Returns 1 if locally held (or available), 0 if remotely held.
 */
static int _skip_remotely_used_lv(struct cmd_context *cmd,
				  const struct logical_volume *lv_top)
{
	int ex, sh;

	if (lv_is_active(lv_top)) {
		ex = 0;
		if (!lockd_query_lv(cmd, lv_top, &ex, &sh) || !ex)
			return 0;
	} else {
		if (!lockd_lv(cmd, (struct logical_volume *)lv_top, "ex", LDLV_PERSISTENT))
			return 0;
		/*
		 * Probe-and-release: the pvmove LV's EX lock (not this LV's
		 * lock) prevents other nodes from activating this LV while
		 * it is being moved.
		 */
		if (!lockd_lv(cmd, (struct logical_volume *)lv_top, "un", LDLV_PERSISTENT))
			log_warn("WARNING: Failed to unlock LV %s after remote check.",
				 display_lvname(lv_top));
	}
	return 1;
}

/*
 * If lv_move is set (named pvmove), a remotely-locked LV is a command error.
 * Otherwise, remotely-locked LVs are skipped with a warning.
 */
static int _skip_remote_lvs(struct cmd_context *cmd,
			    const struct logical_volume *lv_move,
			    struct dm_list *moving_lvs)
{
	const struct logical_volume *lv_top;
	struct lv_list *lvl, *lvl2, *lvl3;
	struct dm_list remote_tops;
	int already_done;

	/* Named pvmove: check the named LV directly */
	if (lv_move) {
		lv_top = lv_lock_holder(lv_move);
		if (!_skip_remotely_used_lv(cmd, lv_top)) {
			log_error("LV %s is locked on another cluster node.",
				  display_lvname(lv_move));
			return 0;
		}
		return 1;
	}

	dm_list_init(&remote_tops);

	dm_list_iterate_items_safe(lvl, lvl2, moving_lvs) {
		lv_top = lv_lock_holder(lvl->lv);

		/* Skip if same lv_top was already checked for a previous entry */
		already_done = 0;
		dm_list_iterate_items(lvl3, moving_lvs) {
			if (lvl3 == lvl)
				break;
			if (lv_lock_holder(lvl3->lv) == lv_top) {
				already_done = 1;
				break;
			}
		}
		if (already_done)
			continue;

		/*
		 * The dedup scan above only finds entries still in moving_lvs.
		 * When a remotely-held lv_top was already removed, later sub-LVs
		 * of the same lv_top would re-query lockd.  Check remote_tops to
		 * avoid those redundant lock round-trips.
		 */
		dm_list_iterate_items(lvl3, &remote_tops) {
			if (lv_lock_holder(lvl3->lv) == lv_top) {
				already_done = 1;
				break;
			}
		}
		if (already_done) {
			log_warn("WARNING: Not moving LV %s. LV %s exclusive lock is not held.",
				 display_lvname(lvl->lv), display_lvname(lv_top));
			dm_list_del(&lvl->list);
			continue;
		}

		if (!_skip_remotely_used_lv(cmd, lv_top)) {
			log_warn("WARNING: Not moving LV %s. LV %s exclusive lock is not held.",
				 display_lvname(lvl->lv), display_lvname(lv_top));
			dm_list_del(&lvl->list);
			if ((lvl3 = dm_pool_alloc(cmd->mem, sizeof(*lvl3)))) {
				lvl3->lv = lvl->lv;
				dm_list_add(&remote_tops, &lvl3->list);
			}
		}
	}

	return 1;
}

/*
 * Insert pvmove mirror segments for each moving LV, then convert
 * the pvmove LV to a mirrored target and split parent segments.
 */
static int _populate_pvmove_lv(struct cmd_context *cmd,
			       struct logical_volume *lv_mirr,
			       struct dm_list *source_pvl,
			       struct dm_list *moving_lvs,
			       struct dm_list *allocatable_pvs,
			       alloc_policy_t alloc,
			       struct dm_list *lvs_changed)
{
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, moving_lvs) {
		if (!_insert_pvmove_mirrors(cmd, lv_mirr, source_pvl,
					    lvl->lv, lvs_changed))
			return_0;
	}

	if (!_finalize_pvmove_lv(cmd, lv_mirr, allocatable_pvs, alloc))
		return_0;

	return 1;
}

/*
 * Validate a resume of an in-progress pvmove: find participating LVs,
 * check that the named LV (if any) matches, warn about ignored args.
 * Returns 1 on success, 0 on error, -1 if the named LV is not part
 * of this pvmove (caller should create a new pvmove for it).
 */
static int _pvmove_validate_resume(struct cmd_context *cmd,
				   struct volume_group *vg,
				   struct logical_volume *lv_mirr,
				   const char *lv_name,
				   int pv_count,
				   const char *pv_name)
{
	struct dm_list *lvs_changed;

	log_print_unless_silent("Detected pvmove in progress for %s.", pv_name);

	if (!(lvs_changed = lvs_using_lv(cmd, vg, lv_mirr))) {
		log_error("ABORTING: Failed to generate list of moving LVs.");
		return 0;
	}

	if (lv_name) {
		struct lv_list *lvl;
		int name_matches = 0;

		dm_list_iterate_items(lvl, lvs_changed)
			if (!strcmp(lvl->lv->name, lv_name) ||
			    _sub_lv_of(lvl->lv, lv_name)) {
				name_matches = 1;
				break;
			}

		if (!name_matches) {
			if (vg_is_shared(vg)) {
				log_print_unless_silent("Existing pvmove for %s does not "
							"include %s, setting up new pvmove.",
							pv_name, lv_name);
				return -1;
			}
			log_error("LV %s is not part of the in-progress pvmove on %s.",
				  lv_name, pv_name);
			return 0;
		}
	}

	if (pv_count || lv_name)
		log_warn("WARNING: Ignoring remaining command line arguments.");

	return 1;
}

/*
 * Called to set up initial pvmove LV only.
 * (Not called after first or any other section completes.)
 */
static int _update_metadata_and_activate(struct logical_volume *lv_mirr, struct dm_list *changed_lvs)
{
	struct lv_list *lvl;
	struct logical_volume *lv = NULL;
	unsigned active_count = 0;
	unsigned inactive_count = 0;

	/*
	 * Find the first active LV from changed_lvs for lv_update_and_reload().
	 * The suspend/resume of an active LV with track_pvmove_deps=1
	 * (via !lv_is_pvmove) discovers and reloads ALL active participating
	 * LVs through the pvmove dependency tree.
	 *
	 * If no LV is active, mirrors exist only in metadata and will
	 * take effect when LVs are activated.
	 */
	dm_list_iterate_items(lvl, changed_lvs) {
		if (lv_is_active(lvl->lv)) {
			if (!lv)
				lv = lvl->lv;
			active_count++;
		} else
			inactive_count++;
	}

	if (!lv) {
		/*
		 * No active LV participates in this pvmove.
		 * Write and commit metadata without suspend/resume:
		 * there are no kernel tables to reload.
		 */
		log_verbose("No active LVs affected by pvmove (%u inactive), "
			    "committing metadata only.", inactive_count);
		if (!vg_write(lv_mirr->vg))
			return_0;
		if (!vg_commit(lv_mirr->vg))
			return_0;
	} else {
		log_verbose("Suspending %s to reload %u active LV%s via pvmove dependency tracking.",
			    display_lvname(lv), active_count, active_count != 1 ? "s" : "");
		if (!lv_update_and_reload(lv))
			return_0;
	}

	/* Activate pvmove mirror to start the data copy */
	if (!activate_lv(lv_mirr->vg->cmd, lv_mirr)) {
		if (test_mode())
			return 1;

		log_error("Temporary pvmove mirror activation failed. Reverting.");

		if (!pvmove_abort_initial(lv_mirr->vg->cmd, lv_mirr->vg,
					  lv_mirr, changed_lvs))
			log_error("ABORTING: Failed to revert pvmove. Run pvmove --abort.");

		return 0;
	}

	return 1;
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

/*
 * Resume an in-progress pvmove: validate, re-acquire cluster lock
 * and activate the pvmove mirror.
 */
static int _pvmove_resume(struct cmd_context *cmd,
			  struct pvmove_params *pp,
			  struct volume_group *vg,
			  struct logical_volume *lv_mirr,
			  const char *lv_name,
			  const char *pv_name,
			  int pv_count)
{
	int val;

	if (!_copy_id_components(cmd, lv_mirr, &pp->id_vg_name, &pp->id_lv_name, pp->lvid))
		return_0;

	val = _pvmove_validate_resume(cmd, vg, lv_mirr, lv_name, pv_count, pv_name);
	if (val <= 0)
		return val;

	/*
	 * In a shared VG, an ex lock on the pvmove LV must be held while pvmove
	 * is moving any data, so this lock must be acquired before activating
	 * lv_mirr at which point data will begin moving again.
	 */
	if (vg_is_shared(vg) && !lockd_lv(cmd, lv_mirr, "ex", LDLV_PERSISTENT)) {
		log_error("ABORTING: Failed to re-acquire cluster lock for pvmove LV.");
		return 0;
	}

	/*
	 * Activating the pvmove LV (lv_mirr) will resume the movement of data.
	 */
	if (!activate_lv(cmd, lv_mirr)) {
		if (vg_is_shared(vg) && !lockd_lv(cmd, lv_mirr, "un", LDLV_PERSISTENT))
			log_warn("WARNING: Failed to unlock pvmove LV");
		log_error("ABORTING: Failed to activate pvmove LV %s.", lv_mirr->name);
		return 0;
	}

	return 1;
}

static struct logical_volume *_create_pvmove_lv(struct cmd_context *cmd,
						struct volume_group *vg)
{
	struct logical_volume *lv_mirr;

	/* FIXME Cope with non-contiguous => splitting existing segments */
	if (!(lv_mirr = lv_create_empty("pvmove%d", NULL,
					LVM_READ | LVM_WRITE,
					ALLOC_CONTIGUOUS, vg))) {
		log_error("Creation of temporary pvmove LV failed.");
		return NULL;
	}

	lv_mirr->status |= (PVMOVE | LOCKED);
	return lv_mirr;
}

static void _lockd_pvmove_undo(struct cmd_context *cmd, struct volume_group *vg,
			       struct logical_volume *lv_mirr, int *lv_mirr_locked)
{
	if (!vg_is_shared(vg) || !lv_mirr)
		return;

	/* _remove_pvmove_lv clears lock_args after lockd cleanup */
	if (!lv_mirr->lock_args)
		return;

	if (*lv_mirr_locked) {
		*lv_mirr_locked = 0;
		if (!lockd_lv(cmd, lv_mirr, "un", LDLV_PERSISTENT))
			log_error("Failed to unlock pvmove LV %s.",
				  display_lvname(lv_mirr));
	}
	if (!lockd_free_lv(cmd, vg, lv_mirr->name, &lv_mirr->lvid.id[1], lv_mirr->lock_args))
		log_error("Failed to free pvmove LV lock.");
}

static int _lockd_pvmove_new(struct cmd_context *cmd, struct volume_group *vg,
				struct logical_volume *lv_mirr, int *lv_mirr_locked)
{
	if (!lockd_init_lv_args(cmd, vg, lv_mirr, vg->lock_type, NULL, &lv_mirr->lock_args)) {
		log_error("Failed to initialize lock for pvmove LV.");
		return 0;
	}
	if (!lockd_lv(cmd, lv_mirr, "ex", LDLV_PERSISTENT)) {
		log_error("Failed to acquire cluster lock for pvmove LV.");
		if (!lockd_free_lv(cmd, vg, lv_mirr->name, &lv_mirr->lvid.id[1], lv_mirr->lock_args))
			log_error("Failed to free pvmove LV lock.");
		return 0;
	}
	*lv_mirr_locked = 1;
	return 1;
}

/*
 * Create a new pvmove: build source/allocatable PV lists, create the
 * temporary pvmove mirror LV, set up mirror segments and activate.
 */
static int _pvmove_create(struct cmd_context *cmd,
			  struct pvmove_params *pp,
			  struct volume_group *vg,
			  struct physical_volume *pv,
			  const char *lv_name)
{
	struct dm_list *source_pvl;
	struct dm_list *allocatable_pvs;
	struct dm_list moving_lvs;
	struct dm_list changed_lvs;
	struct logical_volume *lv_mirr;
	struct logical_volume *lv_move = NULL;
	int lv_mirr_locked = 0;

	if (lv_name && !(lv_move = find_lv(vg, lv_name))) {
		log_error("Logical volume %s not found.", lv_name);
		return 0;
	}

	if (pp->alloc == ALLOC_INHERIT)
		pp->alloc = vg->alloc;

	dm_list_init(&moving_lvs);
	dm_list_init(&changed_lvs);

	/* Determine PE ranges to be moved */
	if (!(source_pvl = create_pv_list(cmd->mem, vg, 1, &pp->pv_name_arg, 0)))
		return_0;

	/* Get PVs we can use for allocation */
	if (!(allocatable_pvs = _get_allocatable_pvs(cmd, pp->pv_count, pp->pv_names, vg, pv, pp->alloc)))
		return_0;

	/* Get LVs that are using the PVs being moved */
	if (!_find_moving_lvs(cmd, vg, source_pvl, lv_move, &moving_lvs))
		return_0;

	/* Skip LVs that are not movable (remove from moving_lvs) */
	if (!_skip_unmovable_lvs(lv_move, source_pvl, &moving_lvs))
		return_0;

	/* Skip LVs that are active on other nodes (remove from moving_lvs) */
	if (vg_is_shared(vg) && !_skip_remote_lvs(cmd, lv_move, &moving_lvs))
		return_0;

	if (dm_list_empty(&moving_lvs)) {
		log_error("All data on source PV skipped. "
			  "It contains only locked or unmovable LVs.");
		return 0;
	}

	/* Create a new mirror LV named "pvmove%d" */
	if (!(lv_mirr = _create_pvmove_lv(cmd, vg)))
		return_0;

	/* Insert mirrors and convert to mirrored target */
	if (!_populate_pvmove_lv(cmd, lv_mirr, source_pvl, &moving_lvs,
				 allocatable_pvs, pp->alloc, &changed_lvs))
		return_0;

	/* Create an "id" that polling will use to refer to this pvmove */
	if (!_copy_id_components(cmd, lv_mirr, &pp->id_vg_name, &pp->id_lv_name, pp->lvid))
		return_0;

	/* Allocate an LV lock for the new pvmove LV, and acquire an ex lock on it */
	if (vg_is_shared(vg) && !_lockd_pvmove_new(cmd, vg, lv_mirr, &lv_mirr_locked))
		return_0;

	/* Write VG metadata, and activate pvmove LV (lv_mirr) to start the data copying */
	if (!_update_metadata_and_activate(lv_mirr, &changed_lvs)) {
		_lockd_pvmove_undo(cmd, vg, lv_mirr, &lv_mirr_locked);
		return_0;
	}

	return 1;
}

static int _pvmove_setup_single(struct cmd_context *cmd,
				struct volume_group *vg,
				struct physical_volume *pv,
				struct processing_handle *handle)
{
	struct pvmove_params *pp = (struct pvmove_params *) handle->custom_handle;
	const char *lv_name = NULL;
	struct logical_volume *lv_mirr;
	const char *pv_name = pv_dev_name(pv);
	int r = ECMD_FAILED;

	if (!vg) {
		log_error(INTERNAL_ERROR "Missing volume group.");
		return r;
	}

	pp->found_pv = 1;
	pp->setup_result = ECMD_FAILED;

	if (pp->lv_name_arg) {
		if (!(lv_name = _extract_lvname(cmd, vg->name, pp->lv_name_arg))) {
			log_error("Failed to find an LV name.");
			pp->setup_result = EINVALID_CMD_LINE;
			return ECMD_FAILED;
		}

		if (!validate_name(lv_name)) {
			log_error("Logical volume name %s is invalid.", lv_name);
			pp->setup_result = EINVALID_CMD_LINE;
			return ECMD_FAILED;
		}
	}

	/*
	 * Resume a pvmove that was previously created, or create a new pvmove.
	 * In a shared VG, _pvmove_resume returns -1 when an existing pvmove
	 * on this PV does not include the named LV; fall through to create
	 * a separate pvmove for it.
	 */
	if ((lv_mirr = find_pvmove_lv(vg, pv_dev(pv), PVMOVE))) {
		int ret = _pvmove_resume(cmd, pp, vg, lv_mirr, lv_name, pv_name, pp->pv_count);
		if (!ret)
			goto_out;
		if (ret < 0) {
			if (!_pvmove_create(cmd, pp, vg, pv, lv_name))
				goto_out;
		}
	} else {
		if (!_pvmove_create(cmd, pp, vg, pv, lv_name))
			goto_out;
	}

	pp->setup_result = ECMD_PROCESSED;
	r = ECMD_PROCESSED;
out:
	return r;
}

static int _pvmove_read_single(struct cmd_context *cmd,
			       struct volume_group *vg,
			       struct physical_volume *pv,
			       struct processing_handle *handle)
{
	struct pvmove_params *pp = (struct pvmove_params *) handle->custom_handle;
	struct logical_volume *lv;
	int ret = ECMD_FAILED;

	if (!vg) {
		log_error(INTERNAL_ERROR "Missing volume group.");
		return ret;
	}

	pp->found_pv = 1;

	if (!(lv = find_pvmove_lv(vg, pv_dev(pv), PVMOVE))) {
		log_print_unless_silent("%s: No pvmove in progress - already finished or aborted.",
					pv_dev_name(pv));
		ret = ECMD_PROCESSED;
		pp->in_progress = 0;
	} else if (_copy_id_components(cmd, lv, &pp->id_vg_name, &pp->id_lv_name, pp->lvid)) {
		ret = ECMD_PROCESSED;
		pp->in_progress = 1;
	}

	return ret;
}

static const struct poll_functions _pvmove_fns = {
	.get_copy_name_from_lv = get_pvmove_pvname_from_lv_mirr,
	.poll_progress = poll_mirror_progress,
	.update_metadata = pvmove_update_metadata,
	.finish_copy = pvmove_finish,
};

int pvmove_poll(struct cmd_context *cmd, const char *pv_name,
		const char *uuid, const char *vg_name,
		const char *lv_name, unsigned background)
{
	const struct poll_operation_id *id = NULL, create_id = {
		.vg_name = vg_name,
		.lv_name = lv_name,
		.display_name = pv_name,
		.uuid = uuid
	};

	if (uuid) {
		if (!vg_name || !lv_name || !pv_name) {
			log_error(INTERNAL_ERROR "Wrong params for creation of pvmove id.");
			return ECMD_FAILED;
		}
		id = &create_id;
	}

	if (test_mode())
		return ECMD_PROCESSED;

	return poll_daemon(cmd, background, PVMOVE, &_pvmove_fns, "Moved", id);
}

int pvmove(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvmove_params pp = { 0 };
	struct processing_handle *handle = NULL;
	union lvid *lvid = NULL;
	char *pv_name = NULL;
	char *colon;
	unsigned is_abort = arg_is_set(cmd, abort_ARG);

	/* dm raid1 target must be present in every case */
	if (!_pvmove_target_present(cmd)) {
		log_error("Required device-mapper target(s) not "
			  "detected in your kernel.");
		return ECMD_FAILED;
	}

	if (lvmlockd_use() && !lvmpolld_use()) {
		/*
		 * Don't want to spend the time making lvmlockd
		 * work without lvmpolld.
		 */
		log_error("Enable lvmpolld when using lvmlockd.");
		return ECMD_FAILED;
	}

	if (lvmlockd_use() && !argc && !is_abort) {
		/*
		 * FIXME: move process_each_vg from polldaemon up to here,
		 * then we can remove this limitation.
		 * Abort without args is allowed: poll_for_all_vgs() iterates
		 * VGs with READ_FOR_UPDATE and finds pvmove LVs via their
		 * cluster locks.
		 */
		log_error("Specify pvmove args when using lvmlockd.");
		return ECMD_FAILED;
	}

	if (argc) {
		if (!(lvid = dm_pool_zalloc(cmd->mem, sizeof(*lvid)))) {
			log_error("Failed to allocate lvid.");
			return ECMD_FAILED;
		}
		pp.lvid = lvid;

		if (!(pp.pv_name_arg = dm_pool_strdup(cmd->mem, argv[0]))) {
			log_error("Failed to clone PV name.");
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

		argc--;
		argv++;

		pp.pv_count = argc;
		pp.pv_names = argv;

		if (arg_is_set(cmd, name_ARG)) {
			if (!(pp.lv_name_arg = dm_pool_strdup(cmd->mem, arg_value(cmd, name_ARG))))  {
				log_error("Failed to clone LV name.");
				return ECMD_FAILED;
			}
		}

		pp.alloc = (alloc_policy_t) (uint32_t) arg_uint_value(cmd, alloc_ARG, ALLOC_INHERIT);

		pp.in_progress = 1;

		/* Normal pvmove setup requires ex lock from lvmlockd. */
		if (is_abort)
			cmd->lockd_vg_default_sh = 1;

		if (!(handle = init_processing_handle(cmd, NULL))) {
			log_error("Failed to initialize processing handle.");
			return ECMD_FAILED;
		}

		handle->custom_handle = &pp;

		process_each_pv(cmd, 1, &pv_name, NULL, 0,
				is_abort ? 0 : READ_FOR_UPDATE,
				handle,
				is_abort ? &_pvmove_read_single : &_pvmove_setup_single);

		destroy_processing_handle(cmd, handle);

		if (!is_abort) {
			if (!pp.found_pv) {
				stack;
				return EINVALID_CMD_LINE;
			}

			if (pp.setup_result != ECMD_PROCESSED) {
				stack;
				return pp.setup_result;
			}
		} else {
			if (!pp.found_pv)
				return_ECMD_FAILED;

			if (!pp.in_progress)
				return ECMD_PROCESSED;
		}

		/*
		 * The command may sit and report progress for some time,
		 * and we do not want or need the global lock held during
		 * that time.
		 */
		lock_global(cmd, "un");
	}

	return pvmove_poll(cmd, pv_name, lvid ? lvid->s : NULL,
			   pp.id_vg_name, pp.id_lv_name,
			   arg_is_set(cmd, background_ARG));
}
