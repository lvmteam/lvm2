/*
 * Copyright (C) 2014-2015 Red Hat, Inc. All rights reserved.
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

#include "lib/misc/lib.h"
#include "lib/metadata/metadata.h"
#include "lib/locking/locking.h"
#include "lib/misc/lvm-string.h"
#include "lib/commands/toolcontext.h"
#include "lib/display/display.h"
#include "lib/metadata/segtype.h"
#include "lib/activate/activate.h"
#include "lib/config/defaults.h"

int lv_is_writecache_origin(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	/*
	 * This flag is needed when removing writecache from an origin
	 * in which case the lv connections have been destroyed and
	 * identifying a writecache origin by these connections doesn't
	 * work.
	 */
	if (lv->status & WRITECACHE_ORIGIN)
		return 1;

	/* Make sure there's exactly one segment in segs_using_this_lv! */
	if (dm_list_size(&lv->segs_using_this_lv) != 1)
		return 0;

	seg = get_only_segment_using_this_lv(lv);
	return seg && lv_is_writecache(seg->lv) && !lv_is_pending_delete(seg->lv) && (seg_lv(seg, 0) == lv);
}

int lv_is_writecache_cachevol(const struct logical_volume *lv)
{
	struct seg_list *sl;

	dm_list_iterate_items(sl, &lv->segs_using_this_lv) {
		if (!sl->seg || !sl->seg->lv || !sl->seg->writecache)
			continue;
		if (lv_is_writecache(sl->seg->lv) && (sl->seg->writecache == lv))
			return 1;
	}
	return 0;
}

static int _get_writecache_kernel_status(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct dm_status_writecache *status_out)
{
	struct lv_with_info_and_seg_status status = {
		.seg_status.type = SEG_STATUS_NONE,
	};

	status.seg_status.seg = first_seg(lv);

	/* FIXME: why reporter_pool? */
	if (!(status.seg_status.mem = dm_pool_create("reporter_pool", 1024))) {
		log_error("Failed to get mem for LV status.");
		return 0;
	}

	if (!lv_info_with_seg_status(cmd, first_seg(lv), &status, 0, 0)) {
		log_error("Failed to get device mapper status for %s", display_lvname(lv));
		goto fail;
	}

	if (!status.info.exists) {
		log_error("No device mapper info exists for %s", display_lvname(lv));
		goto fail;
	}

	if (status.seg_status.type != SEG_STATUS_WRITECACHE) {
		log_error("Invalid device mapper status type (%d) for %s",
			  (uint32_t)status.seg_status.type, display_lvname(lv));
		goto fail;
	}

	status_out->error = status.seg_status.writecache->error;
	status_out->total_blocks = status.seg_status.writecache->total_blocks;
	status_out->free_blocks = status.seg_status.writecache->free_blocks;
	status_out->writeback_blocks = status.seg_status.writecache->writeback_blocks;

	dm_pool_destroy(status.seg_status.mem);
	return 1;

fail:
	dm_pool_destroy(status.seg_status.mem);
	return 0;
}

static int _get_writecache_kernel_error(struct cmd_context *cmd,
					struct logical_volume *lv,
					uint32_t *kernel_error)
{
	struct dm_status_writecache status = { 0 };

	if (!_get_writecache_kernel_status(cmd, lv, &status))
		return_0;

	*kernel_error = status.error;
	return 1;
}

bool lv_writecache_is_clean(struct cmd_context *cmd, struct logical_volume *lv, uint64_t *dirty_blocks)
{
	struct dm_status_writecache status = { 0 };

	if (!_get_writecache_kernel_status(cmd, lv, &status)) 
		return false;

	if (dirty_blocks)
		*dirty_blocks = status.total_blocks - status.free_blocks;

	if (status.total_blocks == status.free_blocks)
		return true;

	return false;
}

static void _rename_detached_cvol(struct cmd_context *cmd, struct logical_volume *lv_fast)
{
	struct volume_group *vg = lv_fast->vg;
	char cvol_name[NAME_LEN];
	char *suffix, *cvol_name_dup;

	/*
	 * Rename lv_fast back to its original name, without the _cvol
	 * suffix that was added when lv_fast was attached for caching.
	 * If the name is in use, generate new lvol%d.
	 * Failing to rename is not really a problem, so we intentionally
	 * do not consider some things here as errors.
	 */
	if (!_dm_strncpy(cvol_name, lv_fast->name, sizeof(cvol_name)) ||
	    !(suffix  = strstr(cvol_name, "_cvol"))) {
		log_debug("LV %s has no suffix for cachevol (skipping rename).",
			display_lvname(lv_fast));
		return;
	}

	*suffix = 0;
	if (lv_name_is_used_in_vg(vg, cvol_name, NULL) &&
	    !generate_lv_name(vg, "lvol%d", cvol_name, sizeof(cvol_name))) {
		log_warn("Failed to generate new unique name for unused LV %s", lv_fast->name);
		return;
	}

	if (!(cvol_name_dup = dm_pool_strdup(vg->vgmem, cvol_name))) {
		stack;
		return;
	}

	lv_fast->name = cvol_name_dup;
}

static int _lv_detach_writecache_cachevol_inactive(struct logical_volume *lv, int noflush)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_fast;
	struct logical_volume *lv_wcorig;
	struct lv_segment *seg = first_seg(lv);
	uint32_t kernel_error = 0;

	if (!seg_is_writecache(seg)) {
		log_error("LV %s segment is not writecache.", display_lvname(lv));
		return 0;
	}

	if (!(lv_fast = seg->writecache)) {
		log_error("LV %s writecache segment has no writecache.", display_lvname(lv));
		return 0;
	}

	if (!(lv_wcorig = seg_lv(seg, 0))) {
		log_error("LV %s writecache segment has no origin", display_lvname(lv));
		return 0;
	}

	if (noflush)
		goto detach;

	/*
	 * Activate LV internally since the LV needs to be active to flush.
	 * LV_TEMPORARY should keep the LV from being exposed to the user
	 * and being accessed.
	 */

	lv->status |= LV_TEMPORARY;

	if (!activate_lv(cmd, lv)) {
		log_error("Failed to activate LV %s for flushing writecache.", display_lvname(lv));
		return 0;
	}

	if (!sync_local_dev_names(cmd)) {
		log_error("Failed to sync local devices before detaching writecache.");
		if (!deactivate_lv(cmd, lv))
			log_error("Failed to deactivate %s.", display_lvname(lv));
		return 0;
	}

	if (!lv_writecache_message(lv, "flush")) {
		log_error("Failed to flush writecache for %s.", display_lvname(lv));
		if (!deactivate_lv(cmd, lv))
			log_error("Failed to deactivate %s.", display_lvname(lv));
		return 0;
	}

	if (!_get_writecache_kernel_error(cmd, lv, &kernel_error)) {
		log_error("Failed to get writecache error status for %s.", display_lvname(lv));
		if (!deactivate_lv(cmd, lv))
			log_error("Failed to deactivate %s.", display_lvname(lv));
		return 0;
	}

	if (kernel_error) {
		log_error("Failed to flush writecache (error %u) for %s.", kernel_error, display_lvname(lv));
		if (!deactivate_lv(cmd, lv))
			log_error("Failed to deactivate %s.", display_lvname(lv));
		return 0;
	}

	if (!deactivate_lv(cmd, lv)) {
		log_error("Failed to deactivate LV %s for detaching writecache.", display_lvname(lv));
		return 0;
	}

	lv->status &= ~LV_TEMPORARY;

 detach:
	if (!remove_seg_from_segs_using_this_lv(lv_fast, seg))
		return_0;

	lv->status &= ~WRITECACHE;
	seg->writecache = NULL;

	if (!remove_layer_from_lv(lv, lv_wcorig))
		return_0;

	if (!lv_remove(lv_wcorig))
		return_0;

	lv_set_visible(lv_fast);
	lv_fast->status &= ~LV_CACHE_VOL;

	_rename_detached_cvol(cmd, lv_fast);

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	return 1;
}

static int _lv_detach_writecache_cachevol_active(struct logical_volume *lv, int noflush)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_fast;
	struct logical_volume *lv_wcorig;
	const struct logical_volume *lv_old;
	struct lv_segment *seg = first_seg(lv);
	uint32_t kernel_error = 0;

	if (!seg_is_writecache(seg)) {
		log_error("LV %s segment is not writecache.", display_lvname(lv));
		return 0;
	}

	if (!(lv_fast = seg->writecache)) {
		log_error("LV %s writecache segment has no writecache.", display_lvname(lv));
		return 0;
	}

	if (!(lv_wcorig = seg_lv(seg, 0))) {
		log_error("LV %s writecache segment has no origin", display_lvname(lv));
		return 0;
	}

	if (noflush)
		goto detach;

	if (!lv_writecache_message(lv, "flush_on_suspend")) {
		log_error("Failed to set flush_on_suspend in writecache detach %s.", display_lvname(lv));
		return 0;
	}

 detach:
	if (!remove_seg_from_segs_using_this_lv(lv_fast, seg)) {
		log_error("Failed to remove seg in writecache detach.");
		return 0;
	}

	lv->status &= ~WRITECACHE;
	seg->writecache = NULL;

	if (!remove_layer_from_lv(lv, lv_wcorig)) {
		log_error("Failed to remove lv layer in writecache detach.");
		return 0;
	}

	/*
	 * vg_write(), suspend_lv(), vg_commit(), resume_lv().
	 * usually done by lv_update_and_reload for an active lv,
	 * but in this case we need to check for writecache errors
	 * after suspend.
	 */

	if (!vg_write(vg)) {
		log_error("Failed to write VG in writecache detach.");
		return 0;
	}

	/*
	 * The version of LV before removal of writecache.  When need to
	 * check for kernel errors based on the old version of LV which
	 * is still present in the kernel.
	 */
	if (!(lv_old = lv_committed(lv))) {
		log_error("Failed to get lv_committed in writecache detach.");
		return 0;
	}

	/*
	 * suspend does not use 'lv' as we know it here, but grabs the
	 * old (precommitted) version of 'lv' using lv_committed(),
	 * which is from vg->vg_comitted.
	 */
	log_debug("Suspending writecache to detach %s", display_lvname(lv));

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to suspend LV in writecache detach.");
		vg_revert(vg);
		return 0;
	}

	log_debug("Checking writecache errors to detach.");

	if (!_get_writecache_kernel_error(cmd, (struct logical_volume *)lv_old, &kernel_error)) {
		log_error("Failed to get writecache error status for %s.", display_lvname(lv_old));
		return 0;
	}

	if (kernel_error) {
		log_error("Failed to flush writecache (error %u) for %s.", kernel_error, display_lvname(lv));
		return 0;
	}

	if (!vg_commit(vg)) {
		log_error("Failed to commit VG in writecache detach.");
		return 0;
	}

	/*
	 * Since vg_commit has happened, vg->vg_committed is now the
	 * newest copy of lv, so resume uses the 'lv' that we know
	 * here.
	 */
	log_debug("Resuming after writecache detached %s", display_lvname(lv));

	if (!resume_lv(cmd, lv)) {
		log_error("Failed to resume LV in writecache detach.");
		return 0;
	}

	log_debug("Deactivating previous cachevol %s", display_lvname(lv_fast));

	if (!deactivate_lv(cmd, lv_fast))
		log_error("Failed to deactivate previous cachevol in writecache detach.");

	/*
	 * Needed for lv_is_writecache_origin to know lv_wcorig was
	 * a writecache origin, which is needed so that the -real
	 * dm uuid suffix is applied, which is needed for deactivate to
	 * work. This is a hacky roundabout way of setting the -real
	 * uuid suffix (it would be nice to have a deactivate command
	 * that accepts a dm uuid.)
	 */
	lv_wcorig->status |= WRITECACHE_ORIGIN;

	log_debug("Deactivating previous wcorig %s", display_lvname(lv_wcorig));

	if (!lv_deactivate(cmd, NULL, lv_wcorig))
		log_error("Failed to deactivate previous wcorig LV in writecache detach.");

	log_debug("Removing previous wcorig %s", display_lvname(lv_wcorig));

	if (!lv_remove(lv_wcorig)) {
		log_error("Failed to remove previous wcorig LV in writecache detach.");
		return 0;
	}

	lv_set_visible(lv_fast);
	lv_fast->status &= ~LV_CACHE_VOL;

	_rename_detached_cvol(cmd, lv_fast);

	if (!vg_write(vg) || !vg_commit(vg)) {
		log_error("Failed to write and commit VG in writecache detach.");
		return 0;
	}

	return 1;
}

int lv_detach_writecache_cachevol(struct logical_volume *lv, int noflush)
{
	if (lv_is_active(lv))
		return _lv_detach_writecache_cachevol_active(lv, noflush);
	else
		return _lv_detach_writecache_cachevol_inactive(lv, noflush);
}

int lv_writecache_set_cleaner(struct logical_volume *lv)
{
	struct lv_segment *seg = first_seg(lv);

	seg->writecache_settings.cleaner = 1;
	seg->writecache_settings.cleaner_set = 1;

	if (lv_is_active(lv)) {
		if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
			log_error("Failed to update VG.");
			return 0;
		}
		if (!lv_writecache_message(lv, "cleaner")) {
			log_error("Failed to set writecache cleaner for %s.", display_lvname(lv));
			return 0;
		}
	} else {
		if (!vg_write(lv->vg) || !vg_commit(lv->vg)) {
			log_error("Failed to update VG.");
			return 0;
		}
	}
	return 1;
}

int writecache_settings_to_str_list(struct writecache_settings *settings, struct dm_list *result, struct dm_pool *mem)
{
	int errors = 0;

	if (settings->high_watermark_set)
		if (!setting_str_list_add("high_watermark", settings->high_watermark, NULL, result, mem))
			errors++;

	if (settings->low_watermark_set)
		if (!setting_str_list_add("low_watermark", settings->low_watermark, NULL, result, mem))
			errors++;

	if (settings->writeback_jobs_set)
		if (!setting_str_list_add("writeback_jobs", settings->writeback_jobs, NULL, result, mem))
			errors++;

	if (settings->autocommit_blocks_set)
		if (!setting_str_list_add("autocommit_blocks", settings->autocommit_blocks, NULL, result, mem))
			errors++;

	if (settings->autocommit_time_set)
		if (!setting_str_list_add("autocommit_time", settings->autocommit_time, NULL, result, mem))
			errors++;

	if (settings->fua_set)
		if (!setting_str_list_add("fua", (uint64_t)settings->fua, NULL, result, mem))
			errors++;

	if (settings->nofua_set)
		if (!setting_str_list_add("nofua", (uint64_t)settings->nofua, NULL, result, mem))
			errors++;

	if (settings->cleaner_set && settings->cleaner)
		if (!setting_str_list_add("cleaner", (uint64_t)settings->cleaner, NULL, result, mem))
			errors++;

	if (settings->max_age_set)
		if (!setting_str_list_add("max_age", (uint64_t)settings->max_age, NULL, result, mem))
			errors++;

	if (settings->metadata_only_set)
		if (!setting_str_list_add("metadata_only", (uint64_t)settings->metadata_only, NULL, result, mem))
			errors++;

	if (settings->pause_writeback_set)
		if (!setting_str_list_add("pause_writeback", (uint64_t)settings->pause_writeback, NULL, result, mem))
			errors++;

	if (settings->new_key && settings->new_val)
		if (!setting_str_list_add(settings->new_key, 0, settings->new_val, result, mem))
			errors++;

	if (errors)
		log_warn("Failed to create list of writecache settings.");

	return 1;
}

