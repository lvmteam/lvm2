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

#include "pvmove_poll.h"
#include "tools.h"

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

/*
 * Called to advance the mirror to successive sections of it.
 * (Not called first time or after the last section completes.)
 */
int pvmove_update_metadata(struct cmd_context *cmd, struct volume_group *vg,
			   struct logical_volume *lv_mirr,
			   struct dm_list *lvs_changed __attribute__((unused)),
			   unsigned flags __attribute__((unused)))
{
	log_verbose("Updating volume group metadata.");
	if (!vg_write(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		return 0;
	}

	if (!suspend_lv(cmd, lv_mirr)) {
		vg_revert(vg);
		log_error("ABORTING: Temporary pvmove mirror reload failed.");
		if (!revert_lv(cmd, lv_mirr))
			stack;
		return 0;
	}

	/* Commit on-disk metadata */
	if (!vg_commit(vg)) {
		log_error("ABORTING: Volume group metadata update failed.");
		if (!resume_lv(cmd, lv_mirr))
			log_error("Unable to reactivate logical volume \"%s\".",
				  lv_mirr->name);
		if (!revert_lv(cmd, lv_mirr))
			stack;
		return 0;
	}

	if (!resume_lv(cmd, lv_mirr)) {
		log_error("Unable to reactivate logical volume \"%s\".",
			  lv_mirr->name);
		return 0;
	}

	backup(vg);

	return 1;
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
