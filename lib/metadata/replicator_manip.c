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
#include "locking.h"
#include "metadata.h"
#include "segtype.h"
#include "toolcontext.h"

/* Add lv as replicator_dev device */
int replicator_dev_add_rimage(struct replicator_device *rdev,
			      struct logical_volume *lv)
{
	if (!lv || !rdev)
		return_0;

	if (lv_is_rimage(lv)) {
		log_error("Logical volume %s is already part of other "
			  "replicator.", lv->name);
		return 0;
	}

	if (rdev->lv) {
		log_error("Logical volume %s can not be attached to an "
			  "already defined replicator device", lv->name);
		return 0;
	}

	lv_set_hidden(lv);
	lv->rdevice = rdev;
	rdev->lv = lv;

	return add_seg_to_segs_using_this_lv(lv, rdev->replicator_dev);
}

/* Remove lv from replicator_dev device */
struct logical_volume *replicator_dev_remove_rimage(struct replicator_device *rdev)
{
	struct logical_volume *lv;

	if (!rdev || !rdev->lv)
		return_NULL;

	lv = rdev->lv;
	if (!remove_seg_from_segs_using_this_lv(lv, rdev->replicator_dev))
		return_NULL;

	/* FIXME: - check for site references */
	rdev->lv = NULL;
	lv->rdevice = NULL;
	lv_set_visible(lv);

	return lv;
}

int replicator_dev_add_slog(struct replicator_device *rdev,
			    struct logical_volume *slog)
{
	if (!slog || !rdev)
		return_0;

	if (rdev->slog) {
		log_error("Replicator device in site %s already has sync log.",
			  rdev->rsite->name);
		return 0;
	}

	if (slog->rdevice) {
		log_error("Sync log %s is already used by replicator %s.",
			  slog->name, slog->rdevice->rsite->replicator->name);
		return 0;
	}

	lv_set_hidden(slog);
	slog->rdevice = rdev;
	rdev->slog = slog;

	return add_seg_to_segs_using_this_lv(slog, rdev->replicator_dev);
}

struct logical_volume *replicator_dev_remove_slog(struct replicator_device *rdev)
{
	struct logical_volume *lv;

	if (!rdev)
		return_NULL;

	lv = rdev->slog;
	if (!lv) {
		log_error("Replicator device in site %s does not have sync log.",
			  rdev->rsite->name);
		return NULL;
	}

	if (!remove_seg_from_segs_using_this_lv(lv, rdev->replicator_dev))
		return_NULL;

	rdev->slog = NULL;
	lv->rdevice = NULL;
	lv_set_visible(lv);

	return lv;
}

int replicator_add_replicator_dev(struct logical_volume *replicator_lv,
				  struct lv_segment *replicator_dev_seg)
{
	if (!replicator_lv)
		return_0;

	if (!(replicator_lv->status & REPLICATOR)) {
		dm_list_init(&replicator_lv->rsites);
		lv_set_hidden(replicator_lv);
		replicator_lv->status |= REPLICATOR;
	}

	if (!replicator_dev_seg)
		return 1;

	if (replicator_dev_seg->replicator) {
		log_error("Replicator device %s is already part of replicator.",
			  replicator_dev_seg->lv->name);
		return 0;
	}

	replicator_dev_seg->replicator = replicator_lv;

	return add_seg_to_segs_using_this_lv(replicator_lv, replicator_dev_seg);
}

/**
 * Returns rimage ?? lv upon succeful detach of device
 * entire LV entry should be removed by this crootall ??
 */
struct logical_volume *replicator_remove_replicator_dev(struct lv_segment *replicator_dev_seg)
{
	struct logical_volume *lv = NULL;

	log_error("FIXME: not implemented.");
#if 0
	/* FIXME: - this is going to be complex.... */
	if (!replicator_dev_seg)
		return_NULL;

	/* if slog or rimage - exit */

	if (!remove_seg_from_segs_using_this_lv(lv, replicator_seg))
		return_NULL;

	replicator_seg->rlog_lv = NULL;
	lv->status &= ~REPLICATOR_LOG;
	lv_set_visible(lv);
#endif

	return lv;
}

int replicator_add_rlog(struct lv_segment *replicator_seg,
			struct logical_volume *rlog_lv)
{
	if (!rlog_lv)
		return_0;

	if (rlog_lv->status & REPLICATOR_LOG) {
		log_error("Rlog device %s is already used.", rlog_lv->name);
		return 0;
	}

	lv_set_hidden(rlog_lv);
	rlog_lv->status |= REPLICATOR_LOG;
	replicator_seg->rlog_lv = rlog_lv;

	return add_seg_to_segs_using_this_lv(rlog_lv, replicator_seg);
}

struct logical_volume *replicator_remove_rlog(struct lv_segment *replicator_seg)
{
	struct logical_volume *lv;

	if (!replicator_seg)
		return_0;

	if (!(lv = replicator_seg->rlog_lv)) {
		log_error("Replog segment %s does not have rlog.",
			  replicator_seg->lv->name);
		return NULL;
	}

	if (!remove_seg_from_segs_using_this_lv(lv, replicator_seg))
		return_NULL;

	replicator_seg->rlog_lv = NULL;
	lv->status &= ~REPLICATOR_LOG;
	lv_set_visible(lv);

	return lv;
}


#if 0
/*
 * Create new LV to pretend the original LV
 * this target will have a 'replicator' segment
 */
int lv_add_replicator(struct logical_volume *origin, const char *rep_suffix)
{
	struct logical_volume *rep_lv;
	char *name;
	size_t slen;

	if (!(name = strstr(origin->name, rep_suffix))) {
		log_error("Failed to find replicator suffix %s in LV name %s",
			  rep_suffix, origin->name);
		return 0;
	}
	slen = (size_t)(name - origin->name);
	name = alloca(slen + 1);
	memcpy(name, origin->name, slen);
	name[slen] = 0;

	if ((rep_lv = find_lv(origin->vg, name))) {
		rep_lv->status |= VIRTUAL;
		return 1;
	}

	if (!(rep_lv = lv_create_empty(name, &origin->lvid,
				       LVM_READ | LVM_WRITE | VISIBLE_LV,
				       ALLOC_INHERIT, origin->vg)))
		return_0;

	if (!lv_add_virtual_segment(rep_lv, 0, origin->le_count,
				    get_segtype_from_string(origin->vg->cmd,
							    "error")))
		return_0;

	rep_lv->status |= VIRTUAL;
	return 1;
}

int lv_remove_replicator(struct logical_volume *lv)
{
	return 1;
}
#endif

/*
 * Check all replicator structures:
 *  only non-clustered VG for Replicator
 *  only one segment in replicator LV
 *  site has correct combination of operation_mode parameters
 *  site and related devices have correct index numbers
 *  duplicate site names, site indexes, device names, device indexes
 */
int check_replicator_segment(const struct lv_segment *rseg)
{
	struct replicator_site *rsite, *rsiteb;
	struct replicator_device *rdev, *rdevb;
        struct logical_volume *lv = rseg->lv;
	int r = 1;

	if (vg_is_clustered(lv->vg)) {
		log_error("Volume Group %s of replicator %s is clustered",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (dm_list_size(&lv->segments) != 1) {
		log_error("Replicator %s segment size %d != 1",
			  lv->name, dm_list_size(&lv->segments));
		return 0;
	}

	dm_list_iterate_items(rsite, &lv->rsites) {
		if (rsite->op_mode == DM_REPLICATOR_SYNC) {
			if (rsite->fall_behind_timeout) {
				log_error("Defined fall_behind_timeout="
					  "%d for sync replicator %s/%s.",
					  rsite->fall_behind_timeout, lv->name,
					  rsite->name);
				r = 0;
			}
			if (rsite->fall_behind_ios) {
				log_error("Defined fall_behind_ios="
					  "%d for sync replicator %s/%s.",
					  rsite->fall_behind_ios, lv->name, rsite->name);
				r = 0;
			}
			if (rsite->fall_behind_data) {
				log_error("Defined fall_behind_data="
					  "%" PRIu64 " for sync replicator %s/%s.",
					  rsite->fall_behind_data, lv->name, rsite->name);
				r = 0;
			}
		} else {
			if (rsite->fall_behind_timeout && rsite->fall_behind_ios) {
				log_error("Defined fall_behind_timeout and"
					  " fall_behind_ios for async replicator %s/%s.",
					  lv->name, rsite->name);
				r = 0;
			}
			if (rsite->fall_behind_timeout && rsite->fall_behind_data) {
				log_error("Defined fall_behind_timeout and"
					  " fall_behind_data for async replicator %s/%s.",
					  lv->name, rsite->name);
				r = 0;
			}
			if (rsite->fall_behind_ios && rsite->fall_behind_data) {
				log_error("Defined fall_behind_ios and"
					  " fall_behind_data for async replicator %s/%s.",
					  lv->name, rsite->name);
				r = 0;
			}
			if (!rsite->fall_behind_ios &&
			    !rsite->fall_behind_data &&
			    !rsite->fall_behind_timeout) {
				log_error("fall_behind_timeout,"
					  " fall_behind_ios and fall_behind_data are"
					  " undefined for async replicator %s/%s.",
					  lv->name, rsite->name);
				r = 0;
			}
		}
		dm_list_iterate_items(rsiteb, &lv->rsites) {
			if (rsite == rsiteb)
				break;
			if (strcasecmp(rsite->name, rsiteb->name) == 0) {
				log_error("Duplicate site name "
					  "%s detected for replicator %s.",
					  rsite->name, lv->name);
				r = 0;
			}
			if ((rsite->vg_name && rsiteb->vg_name &&
			     strcasecmp(rsite->vg_name, rsiteb->vg_name) == 0) ||
			    (!rsite->vg_name && !rsiteb->vg_name)) {
				log_error("Duplicate VG name "
					  "%s detected for replicator %s.",
					  (rsite->vg_name) ? rsite->vg_name : "<local>",
					  lv->name);
				r = 0;
			}
			if (rsite->site_index == rsiteb->site_index) {
				log_error("Duplicate site index %d detected "
					  "for replicator site %s/%s.",
					  rsite->site_index, lv->name,
					  rsite->name);
				r = 0;
			}
			if (rsite->site_index > rseg->rsite_index_highest) {
				log_error("Site index %d > %d (too high) "
					  "for replicator site %s/%s.",
					  rsite->site_index,
					  rseg->rsite_index_highest,
					  lv->name, rsite->name);
				r = 0;
			}
		}

		dm_list_iterate_items(rdev, &rsite->rdevices) {
			dm_list_iterate_items(rdevb, &rsite->rdevices) {
				if (rdev == rdevb)
					break;
				if (rdev->slog && (rdev->slog == rdevb->slog)) {
					log_error("Duplicate sync log %s "
						  "detected for replicator %s.",
						  rdev->slog->name, lv->name);
					r = 0;
				}
				if (strcasecmp(rdev->name, rdevb->name) == 0) {
					log_error("Duplicate device name %s "
						  "detected for replicator %s.",
						  rdev->name, lv->name);
					r = 0;
				}
				if (rdev->device_index == rdevb->device_index) {
					log_error("Duplicate device index %"
						  PRId64 " detected for "
						  "replicator site %s/%s.",
						  rdev->device_index,
						  lv->name, rsite->name);
					r = 0;
				}
				if (rdev->device_index > rseg->rdevice_index_highest) {
					log_error("Device index %" PRIu64
						  " > %" PRIu64 " (too high) "
						  "for replicator site %s/%s.",
						  rdev->device_index,
						  rseg->rdevice_index_highest,
						  lv->name, rsite->name);
					r = 0;
				}
			}
		}
	}

	return r;
}

/**
 * Is this segment part of active replicator
 */
int lv_is_active_replicator_dev(const struct logical_volume *lv)
{
	return ((lv->status & REPLICATOR) &&
		lv->rdevice &&
		lv->rdevice->rsite &&
		lv->rdevice->rsite->state == REPLICATOR_STATE_ACTIVE);
}

/**
 * Is this LV replicator control device
 */
int lv_is_replicator(const struct logical_volume *lv)
{
	return ((lv->status & REPLICATOR) &&
		!dm_list_empty(&lv->segments) &&
		seg_is_replicator(first_seg(lv)));
}

/**
 * Is this LV replicator device
 */
int lv_is_replicator_dev(const struct logical_volume *lv)
{
	return ((lv->status & REPLICATOR) &&
		!dm_list_empty(&lv->segments) &&
		seg_is_replicator_dev(first_seg(lv)));
}

/**
 * Is this LV replicated origin lv
 */
int lv_is_rimage(const struct logical_volume *lv)
{
	return (lv->rdevice && lv->rdevice->lv == lv);
}

/**
 * Is this LV rlog
 */
int lv_is_rlog(const struct logical_volume *lv)
{
	return (lv->status & REPLICATOR_LOG);
}

/**
 * Is this LV sync log
 */
int lv_is_slog(const struct logical_volume *lv)
{
	return (lv->rdevice && lv->rdevice->slog == lv);
}

/**
 * Returns first replicator-dev in site in case the LV is replicator-dev,
 * NULL otherwise
 */
struct logical_volume *first_replicator_dev(const struct logical_volume *lv)
{
	struct replicator_device *rdev;
	struct replicator_site *rsite;

	if (lv_is_replicator_dev(lv))
		dm_list_iterate_items(rsite, &first_seg(lv)->replicator->rsites) {
			dm_list_iterate_items(rdev, &rsite->rdevices)
				return rdev->replicator_dev->lv;
			break;
		}

	return NULL;
}

/**
 * Add VG open parameters to sorted cmd_vg list.
 *
 * Maintain the alphabeticaly ordered list, avoid duplications.
 *
 * \return	Returns newly created or already present cmd_vg entry,
 *		or NULL in error case.
 */
struct cmd_vg *cmd_vg_add(struct dm_pool *mem, struct dm_list *cmd_vgs,
			  const char *vg_name, const char *vgid,
			  uint32_t flags)
{
	struct cmd_vg *cvl, *ins;

	if (!vg_name && !vgid) {
		log_error("Either vg_name or vgid must be set.");
		return NULL;
	}

	/* Is it already in the list ? */
	if ((cvl = cmd_vg_lookup(cmd_vgs, vg_name, vgid)))
		return cvl;

	if (!(cvl = dm_pool_zalloc(mem, sizeof(*cvl)))) {
		log_error("Allocation of cmd_vg failed.");
		return NULL;
	}

	if (vg_name && !(cvl->vg_name = dm_pool_strdup(mem, vg_name))) {
		dm_pool_free(mem, cvl);
		log_error("Allocation of vg_name failed.");
		return NULL;
	}

	if (vgid && !(cvl->vgid = dm_pool_strdup(mem, vgid))) {
		dm_pool_free(mem, cvl);
		log_error("Allocation of vgid failed.");
		return NULL;
	}

	cvl->flags = flags;

	if (vg_name)
		dm_list_iterate_items(ins, cmd_vgs)
			if (strcmp(vg_name, ins->vg_name) < 0) {
				cmd_vgs = &ins->list; /* new position */
				break;
			}

	dm_list_add(cmd_vgs, &cvl->list);

	return cvl;
}

/**
 * Find cmd_vg with given vg_name in cmd_vgs list.
 *
 * \param cmd_vgs	List of cmd_vg entries.
 *
 * \param vg_name	Name of VG to be found.

 * \param vgid		UUID of VG to be found.
 *
 * \return		Returns cmd_vg entry if vg_name or vgid is found,
 *			NULL otherwise.
 */
struct cmd_vg *cmd_vg_lookup(struct dm_list *cmd_vgs,
			     const char *vg_name, const char *vgid)
{
	struct cmd_vg *cvl;

	dm_list_iterate_items(cvl, cmd_vgs)
		if ((vgid && cvl->vgid && !strcmp(vgid, cvl->vgid)) ||
		    (vg_name && cvl->vg_name && !strcmp(vg_name, cvl->vg_name)))
			return cvl;
	return NULL;
}

/**
 * Read and lock multiple VGs stored in cmd_vgs list alphabeticaly.
 * On the success list head pointer is set to VGs' cmd_vgs.
 * (supports FAILED_INCONSISTENT)
 *
 * \param cmd_vg	Contains list of cmd_vg entries.
 *
 * \return		Returns 1 if all VG in cmd_vgs list are correctly
 *			openned and locked, 0 otherwise.
 */
int cmd_vg_read(struct cmd_context *cmd, struct dm_list *cmd_vgs)
{
	struct cmd_vg *cvl;

	/* Iterate through alphabeticaly ordered cmd_vg list */
	dm_list_iterate_items(cvl, cmd_vgs) {
		cvl->vg = vg_read(cmd, cvl->vg_name, cvl->vgid, cvl->flags);
		if (vg_read_error(cvl->vg)) {
			log_debug("Failed to vg_read %s", cvl->vg_name);
			return 0;
		}
		cvl->vg->cmd_vgs = cmd_vgs;	/* Make it usable in VG */
	}

	return 1;
}

/**
 * Release opened and locked VGs from list.
 *
 * \param cmd_vgs	Contains list of cmd_vg entries.
 */
void free_cmd_vgs(struct dm_list *cmd_vgs)
{
	struct cmd_vg *cvl;

	/* Backward iterate cmd_vg list */
	dm_list_iterate_back_items(cvl, cmd_vgs) {
		if (vg_read_error(cvl->vg))
			release_vg(cvl->vg);
		else
			unlock_and_release_vg(cvl->vg->cmd, cvl->vg, cvl->vg_name);
		cvl->vg = NULL;
	}
}

/**
 * Find all needed remote VGs for processing given LV.
 * Missing VGs are added to VG's cmd_vg list and flag cmd_missing_vgs is set.
 */
int find_replicator_vgs(struct logical_volume *lv)
{
	struct replicator_site *rsite;
	int ret = 1;

	if (!lv_is_replicator_dev(lv))
		return 1;

	dm_list_iterate_items(rsite, &first_seg(lv)->replicator->rsites) {
		if (!rsite->vg_name || !lv->vg->cmd_vgs ||
		    cmd_vg_lookup(lv->vg->cmd_vgs, rsite->vg_name, NULL))
			continue;
		ret = 0;
		/* Using cmd memory pool for cmd_vg list allocation */
		if (!cmd_vg_add(lv->vg->cmd->mem, lv->vg->cmd_vgs,
				rsite->vg_name, NULL, 0)) {
			lv->vg->cmd_missing_vgs = 0; /* do not retry */
			stack;
			break;
		}

		log_debug("VG: %s added as missing.", rsite->vg_name);
		lv->vg->cmd_missing_vgs++;
	}

	return ret;
}

/**
 * Read all remote VGs from lv's replicator sites.
 * Function is used in activation context and needs all VGs already locked.
 */
int lv_read_replicator_vgs(struct logical_volume *lv)
{
	struct replicator_device *rdev;
	struct replicator_site *rsite;
	struct volume_group *vg;

	if (!lv_is_replicator_dev(lv))
		return 1;

	dm_list_iterate_items(rsite, &first_seg(lv)->replicator->rsites) {
		if (!rsite->vg_name)
			continue;
		vg = vg_read(lv->vg->cmd, rsite->vg_name, 0, 0); // READ_WITHOUT_LOCK
		if (vg_read_error(vg)) {
			log_error("Unable to read volume group %s",
				  rsite->vg_name);
			goto bad;
		}
		rsite->vg = vg;
		/* FIXME: handling missing LVs needs to be better */
		dm_list_iterate_items(rdev, &rsite->rdevices)
			if (!(rdev->lv = find_lv(vg, rdev->name))) {
				log_error("Unable to find %s in volume group %s",
					  rdev->name, rsite->vg_name);
				goto bad;
			}
	}

	return 1;
bad:
	lv_release_replicator_vgs(lv);
	return 0;
}

/**
 * Release all VG resources taken by lv's replicator sites.
 * Function is used in activation context and needs all VGs already locked.
 */
void lv_release_replicator_vgs(struct logical_volume *lv)
{
	struct replicator_site *rsite;

	if (!lv_is_replicator_dev(lv))
		return;

	dm_list_iterate_back_items(rsite, &first_seg(lv)->replicator->rsites)
		if (rsite->vg_name && rsite->vg) {
			release_vg(rsite->vg);
			rsite->vg = NULL;
		}
}
