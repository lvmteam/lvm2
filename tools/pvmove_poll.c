/*
 * Copyright (C) 2015 Red Hat, Inc. All rights reserved.
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

#include "pvmove_poll.h"
#include "tools.h"

int pvmove_target_present(struct cmd_context *cmd, int clustered)
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

unsigned pvmove_is_exclusive(struct cmd_context *cmd, struct volume_group *vg)
{
	if (vg_is_clustered(vg))
		if (!pvmove_target_present(cmd, 1))
			return 1;

	return 0;
}

struct volume_group *get_vg(struct cmd_context *cmd, const char *vgname)
{
	dev_close_all();

	return vg_read_for_update(cmd, vgname, NULL, 0);
}

int _activate_lv(struct cmd_context *cmd, struct logical_volume *lv_mirr,
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

static int _is_pvmove_image_removable(struct logical_volume *mimage_lv,
				      void *baton)
{
	uint32_t mimage_to_remove = *((uint32_t *)baton);
	struct lv_segment *mirror_seg;

	if (!(mirror_seg = get_only_segment_using_this_lv(mimage_lv))) {
		log_error(INTERNAL_ERROR "%s is not a proper mirror image",
			  mimage_lv->name);
		return 0;
	}

	if (seg_type(mirror_seg, 0) != AREA_LV) {
		log_error(INTERNAL_ERROR "%s is not a pvmove mirror of LV-type",
			  mirror_seg->lv->name);
		return 0;
	}

	if (mimage_to_remove > mirror_seg->area_count) {
		log_error(INTERNAL_ERROR "Mirror image %" PRIu32 " not found in segment",
			  mimage_to_remove);
		return 0;
	}

	if (seg_lv(mirror_seg, mimage_to_remove) == mimage_lv)
		return 1;

	return 0;
}

static int _detach_pvmove_mirror(struct cmd_context *cmd,
				 struct logical_volume *lv_mirr)
{
	uint32_t mimage_to_remove = 0;
	struct dm_list lvs_completed;
	struct lv_list *lvl;

	/* Update metadata to remove mirror segments and break dependencies */
	dm_list_init(&lvs_completed);

	if (arg_is_set(cmd, abort_ARG) &&
	    (seg_type(first_seg(lv_mirr), 0) == AREA_LV))
		mimage_to_remove = 1; /* remove the second mirror leg */

	if (!lv_remove_mirrors(cmd, lv_mirr, 1, 0, _is_pvmove_image_removable, &mimage_to_remove, PVMOVE) ||
	    !remove_layers_for_segments_all(cmd, lv_mirr, PVMOVE,
					    &lvs_completed)) {
		return 0;
	}

	dm_list_iterate_items(lvl, &lvs_completed)
		/* FIXME Assumes only one pvmove at a time! */
		lvl->lv->status &= ~LOCKED;

	return 1;
}

static int _suspend_lvs(struct cmd_context *cmd, unsigned first_time,
			struct logical_volume *lv_mirr,
			struct dm_list *lvs_changed,
			struct volume_group *vg_to_revert)
{
	/*
	 * Suspend lvs_changed the first time.
	 * Suspend mirrors on subsequent calls.
	 */
	if (first_time) {
		if (!suspend_lvs(cmd, lvs_changed, vg_to_revert))
			return_0;
	} else if (!suspend_lv(cmd, lv_mirr)) {
		if (vg_to_revert)
			vg_revert(vg_to_revert);
		return_0;
	}

	return 1;
}

static int _resume_lvs(struct cmd_context *cmd, unsigned first_time,
		       struct logical_volume *lv_mirr,
		       struct dm_list *lvs_changed)
{
	/*
	 * Suspend lvs_changed the first time.
	 * Suspend mirrors on subsequent calls.
	 */

	if (first_time) {
		if (!resume_lvs(cmd, lvs_changed)) {
			log_error("Unable to resume logical volumes");
			return 0;
		}
	} else if (!resume_lv(cmd, lv_mirr)) {
		log_error("Unable to reactivate logical volume \"%s\"",
			  lv_mirr->name);
		return 0;
	}

	return 1;
}

/*
 * Called to set up initial pvmove LV and to advance the mirror
 * to successive sections of it.
 * (Not called after the last section completes.)
 */
int pvmove_update_metadata(struct cmd_context *cmd, struct volume_group *vg,
			   struct logical_volume *lv_mirr,
			   struct dm_list *lvs_changed, unsigned flags)
{
	unsigned exclusive = (flags & PVMOVE_EXCLUSIVE) ? 1 : 0;
	unsigned first_time = (flags & PVMOVE_FIRST_TIME) ? 1 : 0;
	int r = 0;

	log_verbose("Updating volume group metadata");
	if (!vg_write(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		return 0;
	}

	if (!_suspend_lvs(cmd, first_time, lv_mirr, lvs_changed, vg)) {
		log_error("ABORTING: Temporary pvmove mirror %s failed.", first_time ? "activation" : "reload");
		/* FIXME Add a recovery path for first time too. */
		if (!first_time && !revert_lv(cmd, lv_mirr))
			stack;
		return 0;
	}

	/* Commit on-disk metadata */
	if (!vg_commit(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		if (!_resume_lvs(cmd, first_time, lv_mirr, lvs_changed))
			stack;
		if (!first_time && !revert_lv(cmd, lv_mirr))
			stack;
		return 0;
	}

	/* Activate the temporary mirror LV */
	/* Only the first mirror segment gets activated as a mirror */
	/* FIXME: Add option to use a log */
	if (first_time) {
		if (!exclusive && pvmove_is_exclusive(cmd, vg))
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
	}

	r = 1;

out:
	if (!_resume_lvs(cmd, first_time, lv_mirr, lvs_changed))
		r = 0;

	if (r)
		backup(vg);

	return r;
}

int pvmove_finish(struct cmd_context *cmd, struct volume_group *vg,
		  struct logical_volume *lv_mirr, struct dm_list *lvs_changed)
{
	int r = 1;

	if (!dm_list_empty(lvs_changed) &&
	    (!_detach_pvmove_mirror(cmd, lv_mirr) ||
	    !replace_lv_with_error_segment(lv_mirr))) {
		log_error("ABORTING: Removal of temporary mirror failed");
		return 0;
	}

	/* Store metadata without dependencies on mirror segments */
	if (!vg_write(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		return 0;
	}

	/* Suspend LVs changed (implicitly suspends lv_mirr) */
	if (!suspend_lvs(cmd, lvs_changed, vg)) {
		log_error("ABORTING: Locking LVs to remove temporary mirror failed");
		if (!revert_lv(cmd, lv_mirr))
			stack;
		return 0;
	}

	/* Store metadata without dependencies on mirror segments */
	if (!vg_commit(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		if (!revert_lv(cmd, lv_mirr))
			stack;
		if (!revert_lvs(cmd, lvs_changed))
			stack;
		return 0;
	}

	/* Release mirror LV.  (No pending I/O because it's been suspended.) */
	if (!resume_lv(cmd, lv_mirr)) {
		log_error("Unable to reactivate logical volume \"%s\"",
			  lv_mirr->name);
		r = 0;
	}

	/* Unsuspend LVs */
	if (!resume_lvs(cmd, lvs_changed))
		stack;

	/* Deactivate mirror LV */
	if (!deactivate_lv(cmd, lv_mirr)) {
		log_error("ABORTING: Unable to deactivate temporary logical "
			  "volume \"%s\"", lv_mirr->name);
		r = 0;
	}

	log_verbose("Removing temporary pvmove LV");
	if (!lv_remove(lv_mirr)) {
		log_error("ABORTING: Removal of temporary pvmove LV failed");
		return 0;
	}

	/* Store it on disks */
	log_verbose("Writing out final volume group after pvmove");
	if (!vg_write(vg) || !vg_commit(vg)) {
		log_error("ABORTING: Failed to write new data locations "
			  "to disk.");
		return 0;
	}

	/* FIXME backup positioning */
	backup(vg);

	return r;
}

struct volume_group *pvmove_get_copy_vg(struct cmd_context *cmd, const char *name,
					const char *uuid __attribute__((unused)))
{
	struct physical_volume *pv;
	struct volume_group *vg;

	/* Reread all metadata in case it got changed */
	if (!(pv = find_pv_by_name(cmd, name, 0, 0))) {
		log_error("ABORTING: Can't reread PV %s", name);
		/* What more could we do here? */
		return NULL;
	}

	vg = get_vg(cmd, pv_vg_name(pv));
	free_pv_fid(pv);

	return vg;
}
