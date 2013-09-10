/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
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
#include "metadata.h"
#include "display.h"
#include "activate.h"
#include "toolcontext.h"
#include "segtype.h"
#include "str_list.h"

#include <time.h>
#include <sys/utsname.h>

static struct utsname _utsname;
static int _utsinit = 0;

static char *_format_pvsegs(struct dm_pool *mem, const struct lv_segment *seg,
			     int range_format)
{
	unsigned int s;
	const char *name = NULL;
	uint32_t extent = 0;
	char extent_str[32];

	if (!dm_pool_begin_object(mem, 256)) {
		log_error("dm_pool_begin_object failed");
		return NULL;
	}

	for (s = 0; s < seg->area_count; s++) {
		switch (seg_type(seg, s)) {
		case AREA_LV:
			name = seg_lv(seg, s)->name;
			extent = seg_le(seg, s);
			break;
		case AREA_PV:
			name = dev_name(seg_dev(seg, s));
			extent = seg_pe(seg, s);
			break;
		case AREA_UNASSIGNED:
			name = "unassigned";
			extent = 0;
			break;
		default:
			log_error(INTERNAL_ERROR "Unknown area segtype.");
			return NULL;
		}

		if (!dm_pool_grow_object(mem, name, strlen(name))) {
			log_error("dm_pool_grow_object failed");
			return NULL;
		}

		if (dm_snprintf(extent_str, sizeof(extent_str),
				"%s%" PRIu32 "%s",
				range_format ? ":" : "(", extent,
				range_format ? "-"  : ")") < 0) {
			log_error("Extent number dm_snprintf failed");
			return NULL;
		}
		if (!dm_pool_grow_object(mem, extent_str, strlen(extent_str))) {
			log_error("dm_pool_grow_object failed");
			return NULL;
		}

		if (range_format) {
			if (dm_snprintf(extent_str, sizeof(extent_str),
					"%" PRIu32, extent + seg->area_len - 1) < 0) {
				log_error("Extent number dm_snprintf failed");
				return NULL;
			}
			if (!dm_pool_grow_object(mem, extent_str, strlen(extent_str))) {
				log_error("dm_pool_grow_object failed");
				return NULL;
			}
		}

		if ((s != seg->area_count - 1) &&
		    !dm_pool_grow_object(mem, range_format ? " " : ",", 1)) {
			log_error("dm_pool_grow_object failed");
			return NULL;
		}
	}

	if (!dm_pool_grow_object(mem, "\0", 1)) {
		log_error("dm_pool_grow_object failed");
		return NULL;
	}

	return dm_pool_end_object(mem);
}

char *lvseg_devices(struct dm_pool *mem, const struct lv_segment *seg)
{
	return _format_pvsegs(mem, seg, 0);
}

char *lvseg_seg_pe_ranges(struct dm_pool *mem, const struct lv_segment *seg)
{
	return _format_pvsegs(mem, seg, 1);
}

char *lvseg_tags_dup(const struct lv_segment *seg)
{
	return tags_format_and_copy(seg->lv->vg->vgmem, &seg->tags);
}

char *lvseg_segtype_dup(struct dm_pool *mem, const struct lv_segment *seg)
{
	return dm_pool_strdup(mem, seg->segtype->ops->name(seg));
}

char *lvseg_discards_dup(struct dm_pool *mem, const struct lv_segment *seg)
{
	return  dm_pool_strdup(mem, get_pool_discards_name(seg->discards));
}

#ifdef DMEVENTD
#  include "libdevmapper-event.h"
#endif
char *lvseg_monitor_dup(struct dm_pool *mem, const struct lv_segment *seg)
{
	const char *s = "";

#ifdef DMEVENTD
	struct lvinfo info;
	int pending = 0, monitored;
	struct lv_segment *segm = (struct lv_segment *) seg;

	if (lv_is_cow(seg->lv) && !lv_is_merging_cow(seg->lv))
		segm = first_seg(seg->lv->snapshot->lv);
	else if (seg->log_lv)
		segm = first_seg(seg->log_lv);

	// log_debug("Query LV:%s mon:%s segm:%s tgtm:%p  segmon:%d statusm:%d", seg->lv->name, segm->lv->name, segm->segtype->name, segm->segtype->ops->target_monitored, seg_monitored(segm), (int)(segm->status & PVMOVE));
	if (!segm->segtype->ops->target_monitored)
		/* Nothing to do, monitoring not supported */;
	else if (lv_is_cow_covering_origin(seg->lv))
		/* Nothing to do, snapshot already covers origin */;
	else if (!seg_monitored(segm) || (segm->status & PVMOVE))
		s = "not monitored";
	else if (lv_info(seg->lv->vg->cmd, seg->lv, 1, &info, 0, 0) && info.exists) {
		monitored = segm->segtype->ops->
			target_monitored((struct lv_segment*)segm, &pending);
		if (pending)
			s = "pending";
		else
			s = (monitored) ? "monitored" : "not monitored";
	} // else log_debug("Not active");
#endif
	return dm_pool_strdup(mem, s);
}

uint64_t lvseg_chunksize(const struct lv_segment *seg)
{
	uint64_t size;

	if (lv_is_cow(seg->lv))
		size = (uint64_t) find_snapshot(seg->lv)->chunk_size;
	else if (seg_is_thin_pool(seg))
		size = (uint64_t) seg->chunk_size;
	else
		size = UINT64_C(0);

	return size;
}

uint64_t lvseg_start(const struct lv_segment *seg)
{
	return (uint64_t) seg->le * seg->lv->vg->extent_size;
}

uint64_t lvseg_size(const struct lv_segment *seg)
{
	return (uint64_t) seg->len * seg->lv->vg->extent_size;
}

uint32_t lv_kernel_read_ahead(const struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(lv->vg->cmd, lv, 0, &info, 0, 1) || !info.exists)
		return UINT32_MAX;
	return info.read_ahead;
}

char *lv_origin_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	if (lv_is_cow(lv))
		return lv_name_dup(mem, origin_from_cow(lv));

	if (lv_is_thin_volume(lv) && first_seg(lv)->origin)
		return lv_name_dup(mem, first_seg(lv)->origin);

	if (lv_is_thin_volume(lv) && first_seg(lv)->external_lv)
		return lv_name_dup(mem, first_seg(lv)->external_lv);

	return NULL;
}

char *lv_name_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	return dm_pool_strdup(mem, lv->name);
}

char *lv_modules_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	struct dm_list *modules;

	if (!(modules = str_list_create(mem))) {
		log_error("modules str_list allocation failed");
		return NULL;
	}

	if (!list_lv_modules(mem, lv, modules))
		return_NULL;
	return tags_format_and_copy(mem, modules);
}

char *lv_mirror_log_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	struct lv_segment *seg;

	dm_list_iterate_items(seg, &lv->segments)
		if (seg_is_mirrored(seg) && seg->log_lv)
			return dm_pool_strdup(mem, seg->log_lv->name);

	return NULL;
}

char *lv_pool_lv_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	struct lv_segment *seg;

	dm_list_iterate_items(seg, &lv->segments)
		if (seg_is_thin_volume(seg) && seg->pool_lv)
			return dm_pool_strdup(mem, seg->pool_lv->name);

	return NULL;
}

char *lv_data_lv_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	struct lv_segment *seg = lv_is_thin_pool(lv) ? first_seg(lv) : NULL;

	return seg ? dm_pool_strdup(mem, seg_lv(seg, 0)->name) : NULL;
}

char *lv_metadata_lv_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	struct lv_segment *seg = lv_is_thin_pool(lv) ? first_seg(lv) : NULL;

	return seg ? dm_pool_strdup(mem, seg->metadata_lv->name) : NULL;
}

const char *lv_layer(const struct logical_volume *lv)
{
	if (lv_is_thin_pool(lv))
		return "tpool";
	else if (lv_is_origin(lv) || lv_is_external_origin(lv))
		return "real";

	return NULL;
}

int lv_kernel_minor(const struct logical_volume *lv)
{
	struct lvinfo info;

	if (lv_info(lv->vg->cmd, lv, 0, &info, 0, 0) && info.exists)
		return info.minor;
	return -1;
}

int lv_kernel_major(const struct logical_volume *lv)
{
	struct lvinfo info;
	if (lv_info(lv->vg->cmd, lv, 0, &info, 0, 0) && info.exists)
		return info.major;
	return -1;
}

char *lv_convert_lv_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	struct lv_segment *seg;

	if (lv->status & (CONVERTING|MIRRORED)) {
		seg = first_seg(lv);

		/* Temporary mirror is always area_num == 0 */
		if (seg_type(seg, 0) == AREA_LV &&
		    is_temporary_mirror_layer(seg_lv(seg, 0)))
			return dm_pool_strdup(mem, seg_lv(seg, 0)->name);
	}
	return NULL;
}

char *lv_move_pv_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	struct lv_segment *seg;

	dm_list_iterate_items(seg, &lv->segments)
		if (seg->status & PVMOVE)
			return dm_pool_strdup(mem, dev_name(seg_dev(seg, 0)));

	return NULL;
}

uint64_t lv_origin_size(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	if (lv_is_cow(lv))
		return (uint64_t) find_snapshot(lv)->len * lv->vg->extent_size;

	if (lv_is_thin_volume(lv) && (seg = first_seg(lv)) &&
	    seg->external_lv)
		return seg->external_lv->size;

	if (lv_is_origin(lv))
		return lv->size;

	return 0;
}

uint64_t lv_metadata_size(const struct logical_volume *lv)
{
	struct lv_segment *seg = lv_is_thin_pool(lv) ? first_seg(lv) : NULL;

	return seg ? seg->metadata_lv->size : 0;
}

char *lv_path_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	char *repstr;
	size_t len;

	if (!*lv->vg->name)
		return dm_pool_strdup(mem, "");

	len = strlen(lv->vg->cmd->dev_dir) + strlen(lv->vg->name) +
		strlen(lv->name) + 2;

	if (!(repstr = dm_pool_zalloc(mem, len))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, len, "%s%s/%s",
			lv->vg->cmd->dev_dir, lv->vg->name, lv->name) < 0) {
		log_error("lvpath snprintf failed");
		return 0;
	}

	return repstr;
}

char *lv_uuid_dup(const struct logical_volume *lv)
{
	return id_format_and_copy(lv->vg->vgmem, &lv->lvid.id[1]);
}

char *lv_tags_dup(const struct logical_volume *lv)
{
	return tags_format_and_copy(lv->vg->vgmem, &lv->tags);
}

uint64_t lv_size(const struct logical_volume *lv)
{
	return lv->size;
}

static int _lv_mimage_in_sync(const struct logical_volume *lv)
{
	percent_t percent;
	struct lv_segment *mirror_seg = find_mirror_seg(first_seg(lv));

	if (!(lv->status & MIRROR_IMAGE) || !mirror_seg)
		return_0;

	if (!lv_mirror_percent(lv->vg->cmd, mirror_seg->lv, 0, &percent,
			       NULL))
		return_0;

	return (percent == PERCENT_100) ? 1 : 0;
}

static int _lv_raid_image_in_sync(const struct logical_volume *lv)
{
	unsigned s;
	percent_t percent;
	char *raid_health;
	struct lv_segment *raid_seg;

	/*
	 * If the LV is not active locally,
	 * it doesn't make sense to check status
	 */
	if (!lv_is_active_locally(lv))
		return 0;  /* Assume not in-sync */

	if (!(lv->status & RAID_IMAGE)) {
		log_error(INTERNAL_ERROR "%s is not a RAID image", lv->name);
		return 0;
	}

	raid_seg = get_only_segment_using_this_lv(first_seg(lv)->lv);
	if (!raid_seg) {
		log_error("Failed to find RAID segment for %s", lv->name);
		return 0;
	}

	if (!seg_is_raid(raid_seg)) {
		log_error("%s on %s is not a RAID segment",
			  raid_seg->lv->name, lv->name);
		return 0;
	}

	if (!lv_raid_percent(raid_seg->lv, &percent))
		return_0;

	if (percent == PERCENT_100)
		return 1;

	/* Find out which sub-LV this is. */
	for (s = 0; s < raid_seg->area_count; s++)
		if (seg_lv(raid_seg, s) == lv)
			break;
	if (s == raid_seg->area_count) {
		log_error(INTERNAL_ERROR
			  "sub-LV %s was not found in raid segment",
			  lv->name);
		return 0;
	}

	if (!lv_raid_dev_health(raid_seg->lv, &raid_health))
		return_0;

	if (raid_health[s] == 'A')
		return 1;

	return 0;
}

/*
 * _lv_raid_healthy
 * @lv: A RAID_IMAGE, RAID_META, or RAID logical volume.
 *
 * Returns: 1 if healthy, 0 if device is not health
 */
static int _lv_raid_healthy(const struct logical_volume *lv)
{
	unsigned s;
	char *raid_health;
	struct lv_segment *raid_seg;

	/*
	 * If the LV is not active locally,
	 * it doesn't make sense to check status
	 */
	if (!lv_is_active_locally(lv))
		return 1;  /* assume healthy */

	if (!lv_is_raid_type(lv)) {
		log_error(INTERNAL_ERROR "%s is not of RAID type", lv->name);
		return 0;
	}

	if (lv->status & RAID)
		raid_seg = first_seg(lv);
	else
		raid_seg = get_only_segment_using_this_lv(first_seg(lv)->lv);

	if (!raid_seg) {
		log_error("Failed to find RAID segment for %s", lv->name);
		return 0;
	}

	if (!seg_is_raid(raid_seg)) {
		log_error("%s on %s is not a RAID segment",
			  raid_seg->lv->name, lv->name);
		return 0;
	}

	if (!lv_raid_dev_health(raid_seg->lv, &raid_health))
		return_0;

	if (lv->status & RAID) {
		if (strchr(raid_health, 'D'))
			return 0;
		else
			return 1;
	}

	/* Find out which sub-LV this is. */
	for (s = 0; s < raid_seg->area_count; s++)
		if (((lv->status & RAID_IMAGE) && (seg_lv(raid_seg, s) == lv)) ||
		    ((lv->status & RAID_META) && (seg_metalv(raid_seg,s) == lv)))
			break;
	if (s == raid_seg->area_count) {
		log_error(INTERNAL_ERROR
			  "sub-LV %s was not found in raid segment",
			  lv->name);
		return 0;
	}

	if (raid_health[s] == 'D')
		return 0;

	return 1;
}

char *lv_attr_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	percent_t snap_percent;
	struct lvinfo info;
	struct lv_segment *seg;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(mem, 11))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	/* Blank if this is a "free space" LV. */
	if (!*lv->name)
		goto out;

	if (lv->status & PVMOVE)
		repstr[0] = 'p';
	else if (lv->status & CONVERTING)
		repstr[0] = 'c';
	/* Origin takes precedence over mirror and thin volume */
	else if (lv_is_origin(lv) || lv_is_external_origin(lv))
		repstr[0] = (lv_is_merging_origin(lv)) ? 'O' : 'o';
	else if (lv_is_thin_pool_metadata(lv) ||
		 lv_is_pool_metadata_spare(lv) ||
		 (lv->status & RAID_META))
		repstr[0] = 'e';
	else if (lv->status & RAID)
		repstr[0] = (lv->status & LV_NOTSYNCED) ? 'R' : 'r';
	else if (lv->status & MIRRORED)
		repstr[0] = (lv->status & LV_NOTSYNCED) ? 'M' : 'm';
	else if (lv_is_thin_volume(lv))
		repstr[0] = 'V';
	else if (lv->status & VIRTUAL)
		repstr[0] = 'v';
	else if (lv_is_thin_pool(lv))
		repstr[0] = 't';
	else if (lv_is_thin_pool_data(lv))
		repstr[0] = 'T';
	else if (lv->status & MIRROR_IMAGE)
		repstr[0] = (_lv_mimage_in_sync(lv)) ? 'i' : 'I';
	else if (lv->status & RAID_IMAGE)
		repstr[0] = (_lv_raid_image_in_sync(lv)) ? 'i' : 'I';
	else if (lv->status & MIRROR_LOG)
		repstr[0] = 'l';
	else if (lv_is_cow(lv))
		repstr[0] = (lv_is_merging_cow(lv)) ? 'S' : 's';
	else
		repstr[0] = '-';

	if (lv->status & PVMOVE)
		repstr[1] = '-';
	else if (lv->status & LVM_WRITE)
		repstr[1] = 'w';
	else if (lv->status & LVM_READ)
		repstr[1] = 'r';
	else
		repstr[1] = '-';

	repstr[2] = alloc_policy_char(lv->alloc);

	if (lv->status & LOCKED)
		repstr[2] = toupper(repstr[2]);

	repstr[3] = (lv->status & FIXED_MINOR) ? 'm' : '-';

	if (lv_info(lv->vg->cmd, lv, 0, &info, 1, 0) && info.exists) {
		if (info.suspended)
			repstr[4] = 's';	/* Suspended */
		else if (info.live_table)
			repstr[4] = 'a';	/* Active */
		else if (info.inactive_table)
			repstr[4] = 'i';	/* Inactive with table */
		else
			repstr[4] = 'd';	/* Inactive without table */

		/* Snapshot dropped? */
		if (info.live_table && lv_is_cow(lv)) {
			if (!lv_snapshot_percent(lv, &snap_percent) ||
			    snap_percent == PERCENT_INVALID) {
				if (info.suspended)
					repstr[4] = 'S'; /* Susp Inv snapshot */
				else
					repstr[4] = 'I'; /* Invalid snapshot */
			}
			else if (snap_percent == PERCENT_MERGE_FAILED) {
				if (info.suspended)
					repstr[4] = 'M'; /* Susp snapshot merge failed */
				else
					repstr[4] = 'm'; /* snapshot merge failed */
			}
		}

		/*
		 * 'R' indicates read-only activation of a device that
		 * does not have metadata flagging it as read-only.
		 */
		if (repstr[1] != 'r' && info.read_only)
			repstr[1] = 'R';

		repstr[5] = (info.open_count) ? 'o' : '-';
	} else {
		repstr[4] = '-';
		repstr[5] = '-';
	}

	if (lv_is_thin_pool(lv) || lv_is_thin_volume(lv))
		repstr[6] = 't';
	else if (lv_is_raid_type(lv))
		repstr[6] = 'r';
	else if (lv_is_mirror_type(lv))
		repstr[6] = 'm';
	else if (lv_is_cow(lv) || lv_is_origin(lv))
		repstr[6] = 's';
	else if (lv_has_unknown_segments(lv))
		repstr[6] = 'u';
	else if (lv_is_virtual(lv))
		repstr[6] = 'v';
	else
		repstr[6] = '-';

	if (((lv_is_thin_volume(lv) && (seg = first_seg(lv)) && seg->pool_lv && (seg = first_seg(seg->pool_lv))) ||
	     (lv_is_thin_pool(lv) && (seg = first_seg(lv)))) &&
	    seg->zero_new_blocks)
		repstr[7] = 'z';
	else
		repstr[7] = '-';

	repstr[8] = '-';
	if (lv->status & PARTIAL_LV)
		repstr[8] = 'p';
	else if (lv_is_raid_type(lv)) {
		uint64_t n;
		if (!_lv_raid_healthy(lv))
			repstr[8] = 'r';  /* RAID needs 'r'efresh */
		else if (lv->status & RAID) {
			if (lv_raid_mismatch_count(lv, &n) && n)
				repstr[8] = 'm';  /* RAID has 'm'ismatches */
		} else if (lv->status & LV_WRITEMOSTLY)
			repstr[8] = 'w';  /* sub-LV has 'w'ritemostly */
	}

	if (lv->status & LV_ACTIVATION_SKIP)
		repstr[9] = 'k';
	else
		repstr[9] = '-';

out:
	return repstr;
}

int lv_set_creation(struct logical_volume *lv,
		    const char *hostname, uint64_t timestamp)
{
	const char *hn;

	if (!hostname) {
		if (!_utsinit) {
			if (uname(&_utsname)) {
				log_error("uname failed: %s", strerror(errno));
				memset(&_utsname, 0, sizeof(_utsname));
			}

			_utsinit = 1;
		}

		hostname = _utsname.nodename;
	}

	if (!(hn = dm_hash_lookup(lv->vg->hostnames, hostname))) {
		if (!(hn = dm_pool_strdup(lv->vg->vgmem, hostname))) {
			log_error("Failed to duplicate hostname");
			return 0;
		}

		if (!dm_hash_insert(lv->vg->hostnames, hostname, (void*)hn))
			return_0;
	}

	lv->hostname = hn;
	lv->timestamp = timestamp ? : (uint64_t) time(NULL);

	return 1;
}

char *lv_time_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	char buffer[50];
	struct tm *local_tm;
	time_t ts = (time_t)lv->timestamp;

	if (!ts ||
	    !(local_tm = localtime(&ts)) ||
	    /* FIXME: make this lvm.conf configurable */
	    !strftime(buffer, sizeof(buffer),
		      "%Y-%m-%d %T %z", local_tm))
		buffer[0] = 0;

	return dm_pool_strdup(mem, buffer);
}

char *lv_host_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	return dm_pool_strdup(mem, lv->hostname ? : "");
}

int lv_active_change(struct cmd_context *cmd, struct logical_volume *lv,
		     enum activation_change activate)
{
	if (activate == CHANGE_AN) {
		log_verbose("Deactivating logical volume \"%s\"", lv->name);
		if (!deactivate_lv(cmd, lv))
			return_0;
	} else if ((activate == CHANGE_AE) ||
		   seg_is_raid(first_seg(lv)) ||
		   lv_is_origin(lv) ||
		   lv_is_thin_type(lv)) {
		if (activate == CHANGE_ALN) {
			/* origin, thin or RAID - all others have _AE */
			/* other types of activation are implicitly exclusive */
			/* Note: the order of tests is mandatory */
			log_error("Cannot deactivate \"%s\" locally.", lv->name);
			return 0;
		}
		log_verbose("Activating logical volume \"%s\" exclusively.",
			    lv->name);
		if (!activate_lv_excl(cmd, lv))
			return_0;
	} else if (activate == CHANGE_ALN) {
		log_verbose("Deactivating logical volume \"%s\" locally.",
			    lv->name);
		if (!deactivate_lv_local(cmd, lv))
			return_0;
	} else if ((activate == CHANGE_ALY) || (activate == CHANGE_AAY)) {
		log_verbose("Activating logical volume \"%s\" locally.",
			    lv->name);
		if (!activate_lv_local(cmd, lv))
			return_0;
	} else { /* CHANGE_AY */
		log_verbose("Activating logical volume \"%s\".", lv->name);
		if (!activate_lv(cmd, lv))
			return_0;
	}

	return 1;
}

char *lv_active_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	const char *s;

	if (vg_is_clustered(lv->vg)) {
		//const struct logical_volume *lvo = lv;
		lv = lv_lock_holder(lv);
		//log_debug("Holder for %s => %s.", lvo->name, lv->name);
	}

	if (!lv_is_active(lv))
		s = ""; /* not active */
	else if (!vg_is_clustered(lv->vg))
		s = "active";
	else if (lv_is_active_exclusive(lv))
		/* exclusive cluster activation */
		s = lv_is_active_exclusive_locally(lv) ?
			"local exclusive" : "remote exclusive";
	else /* locally active */
		s = lv_is_active_but_not_locally(lv) ?
			"remotely" : "locally";

	return dm_pool_strdup(mem, s);
}

char *lv_profile_dup(struct dm_pool *mem, const struct logical_volume *lv)
{
	const char *profile_name = lv->profile ? lv->profile->name : "";
	return dm_pool_strdup(mem, profile_name);
}

/* For given LV find recursively the LV which holds lock for it */
const struct logical_volume *lv_lock_holder(const struct logical_volume *lv)
{
	const struct seg_list *sl;

	if (lv_is_cow(lv))
		return lv_lock_holder(origin_from_cow(lv));

	if (lv_is_thin_pool(lv))
		/* Find any active LV from the pool */
		dm_list_iterate_items(sl, &lv->segs_using_this_lv)
			if (lv_is_active(sl->seg->lv)) {
				log_debug("Thin volume \"%s\" is active.", sl->seg->lv->name);
				return sl->seg->lv;
			}

	/* For other types, by default look for the first user */
	dm_list_iterate_items(sl, &lv->segs_using_this_lv) {
		/* FIXME: complete this exception list */
		if (lv_is_thin_volume(lv) &&
		    lv_is_thin_volume(sl->seg->lv) &&
		    first_seg(lv)->pool_lv == sl->seg->pool_lv)
			continue; /* Skip thin snaphost */
		if (lv_is_external_origin(lv) &&
		    lv_is_thin_volume(sl->seg->lv))
			continue; /* Skip external origin */
		return lv_lock_holder(sl->seg->lv);
	}

	return lv;
}

struct profile *lv_config_profile(const struct logical_volume *lv)
{
	return lv->profile ? : lv->vg->profile;
}
