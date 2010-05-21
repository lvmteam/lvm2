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
#include "metadata.h"
#include "segtype.h"

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
