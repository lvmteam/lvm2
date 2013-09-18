/*
 * Copyright (C) 2010-2013 Red Hat, Inc. All rights reserved.
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
#include "properties.h"
#include "activate.h"
#include "lvm-types.h"
#include "metadata.h"


#define GET_VG_NUM_PROPERTY_FN(NAME, VALUE) \
	GET_NUM_PROPERTY_FN(NAME, VALUE, volume_group, vg)
#define GET_PV_NUM_PROPERTY_FN(NAME, VALUE) \
	GET_NUM_PROPERTY_FN(NAME, VALUE, physical_volume, pv)
#define GET_LV_NUM_PROPERTY_FN(NAME, VALUE) \
	GET_NUM_PROPERTY_FN(NAME, VALUE, logical_volume, lv)
#define GET_LVSEG_NUM_PROPERTY_FN(NAME, VALUE) \
	GET_NUM_PROPERTY_FN(NAME, VALUE, lv_segment, lvseg)
#define GET_PVSEG_NUM_PROPERTY_FN(NAME, VALUE) \
	GET_NUM_PROPERTY_FN(NAME, VALUE, pv_segment, pvseg)

#define SET_VG_NUM_PROPERTY_FN(NAME, SETFN) \
	SET_NUM_PROPERTY_FN(NAME, SETFN, volume_group, vg)
#define SET_PV_NUM_PROPERTY_FN(NAME, SETFN) \
	SET_NUM_PROPERTY_FN(NAME, SETFN, physical_volume, pv)
#define SET_LV_NUM_PROPERTY_FN(NAME, SETFN) \
	SET_NUM_PROPERTY_FN(NAME, SETFN, logical_volume, lv)

#define GET_VG_STR_PROPERTY_FN(NAME, VALUE) \
	GET_STR_PROPERTY_FN(NAME, VALUE, volume_group, vg)
#define GET_PV_STR_PROPERTY_FN(NAME, VALUE) \
	GET_STR_PROPERTY_FN(NAME, VALUE, physical_volume, pv)
#define GET_LV_STR_PROPERTY_FN(NAME, VALUE) \
	GET_STR_PROPERTY_FN(NAME, VALUE, logical_volume, lv)
#define GET_LVSEG_STR_PROPERTY_FN(NAME, VALUE) \
	GET_STR_PROPERTY_FN(NAME, VALUE, lv_segment, lvseg)
#define GET_PVSEG_STR_PROPERTY_FN(NAME, VALUE) \
	GET_STR_PROPERTY_FN(NAME, VALUE, pv_segment, pvseg)

static percent_t _copy_percent(const struct logical_volume *lv)
{
	percent_t percent;

	if (!lv_mirror_percent(lv->vg->cmd, lv, 0, &percent, NULL))
		percent = PERCENT_INVALID;

	return percent;
}

static uint64_t _raidmismatchcount(const struct logical_volume *lv)
{
	uint64_t cnt;

	if (!lv_raid_mismatch_count(lv, &cnt))
		return 0;
	return cnt;
}

static char *_raidsyncaction(const struct logical_volume *lv)
{
	char *action;

	if (!lv_raid_sync_action(lv, &action))
		return 0;

	return action;
}

static uint32_t _raidwritebehind(const struct logical_volume *lv)
{
	return first_seg(lv)->writebehind;
}

static uint32_t _raidminrecoveryrate(const struct logical_volume *lv)
{
	return first_seg(lv)->min_recovery_rate;
}

static uint32_t _raidmaxrecoveryrate(const struct logical_volume *lv)
{
	return first_seg(lv)->max_recovery_rate;
}

static percent_t _snap_percent(const struct logical_volume *lv)
{
	percent_t percent;

	if (!lv_is_cow(lv) || !lv_snapshot_percent(lv, &percent))
		percent = PERCENT_INVALID;

	return percent;
}

static percent_t _data_percent(const struct logical_volume *lv)
{
	percent_t percent;

	if (lv_is_cow(lv))
		return _snap_percent(lv);

	if (lv_is_thin_volume(lv))
		return lv_thin_percent(lv, 0, &percent) ? percent : PERCENT_INVALID;

	return lv_thin_pool_percent(lv, 0, &percent) ? percent : PERCENT_INVALID;
}

static percent_t _metadata_percent(const struct logical_volume *lv)
{
	percent_t percent;

	return lv_thin_pool_percent(lv, 1, &percent) ? percent : PERCENT_INVALID;
}

/* PV */
GET_PV_STR_PROPERTY_FN(pv_fmt, pv_fmt_dup(pv))
#define _pv_fmt_set prop_not_implemented_set
GET_PV_STR_PROPERTY_FN(pv_uuid, pv_uuid_dup(pv))
#define _pv_uuid_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(dev_size, SECTOR_SIZE * pv_dev_size(pv))
#define _dev_size_set prop_not_implemented_set
GET_PV_STR_PROPERTY_FN(pv_name, pv_name_dup(pv))
#define _pv_name_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_mda_free, SECTOR_SIZE * pv_mda_free(pv))
#define _pv_mda_free_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_mda_size, SECTOR_SIZE * pv_mda_size(pv))
#define _pv_mda_size_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pe_start, SECTOR_SIZE * pv->pe_start)
#define _pe_start_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_size, SECTOR_SIZE * pv_size_field(pv))
#define _pv_size_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_free, SECTOR_SIZE * pv_free(pv))
#define _pv_free_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_used, SECTOR_SIZE * pv_used(pv))
#define _pv_used_set prop_not_implemented_set
GET_PV_STR_PROPERTY_FN(pv_attr, pv_attr_dup(pv->vg->vgmem, pv))
#define _pv_attr_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_pe_count, pv->pe_count)
#define _pv_pe_count_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_pe_alloc_count, pv->pe_alloc_count)
#define _pv_pe_alloc_count_set prop_not_implemented_set
GET_PV_STR_PROPERTY_FN(pv_tags, pv_tags_dup(pv))
#define _pv_tags_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_mda_count, pv_mda_count(pv))
#define _pv_mda_count_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_mda_used_count, pv_mda_used_count(pv))
#define _pv_mda_used_count_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_ba_start, SECTOR_SIZE * pv->ba_start)
#define _pv_ba_start_set prop_not_implemented_set
GET_PV_NUM_PROPERTY_FN(pv_ba_size, SECTOR_SIZE * pv->ba_size)
#define _pv_ba_size_set prop_not_implemented_set

/* LV */
GET_LV_STR_PROPERTY_FN(lv_uuid, lv_uuid_dup(lv))
#define _lv_uuid_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_name, lv_name_dup(lv->vg->vgmem, lv))
#define _lv_name_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_path, lv_path_dup(lv->vg->vgmem, lv))
#define _lv_path_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_attr, lv_attr_dup(lv->vg->vgmem, lv))
#define _lv_attr_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(lv_major, lv->major)
#define _lv_major_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(lv_minor, lv->minor)
#define _lv_minor_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(lv_read_ahead, lv->read_ahead * SECTOR_SIZE)
#define _lv_read_ahead_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(lv_kernel_major, lv_kernel_major(lv))
#define _lv_kernel_major_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(lv_kernel_minor, lv_kernel_minor(lv))
#define _lv_kernel_minor_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(lv_kernel_read_ahead, lv_kernel_read_ahead(lv) * SECTOR_SIZE)
#define _lv_kernel_read_ahead_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(lv_size, lv->size * SECTOR_SIZE)
#define _lv_size_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(seg_count, dm_list_size(&lv->segments))
#define _seg_count_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(origin, lv_origin_dup(lv->vg->vgmem, lv))
#define _origin_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(origin_size, (SECTOR_SIZE * lv_origin_size(lv)))
#define _origin_size_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(snap_percent, _snap_percent(lv))
#define _snap_percent_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(copy_percent, _copy_percent(lv))
#define _copy_percent_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(sync_percent, _copy_percent(lv))
#define _sync_percent_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(raid_mismatch_count, _raidmismatchcount(lv))
#define _raid_mismatch_count_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(raid_write_behind, _raidwritebehind(lv))
#define _raid_write_behind_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(raid_min_recovery_rate, _raidminrecoveryrate(lv))
#define _raid_min_recovery_rate_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(raid_max_recovery_rate, _raidmaxrecoveryrate(lv))
#define _raid_max_recovery_rate_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(raid_sync_action, _raidsyncaction(lv))
#define _raid_sync_action_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(move_pv, lv_move_pv_dup(lv->vg->vgmem, lv))
#define _move_pv_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(convert_lv, lv_convert_lv_dup(lv->vg->vgmem, lv))
#define _convert_lv_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_tags, lv_tags_dup(lv))
#define _lv_tags_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(mirror_log, lv_mirror_log_dup(lv->vg->vgmem, lv))
#define _mirror_log_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_modules, lv_modules_dup(lv->vg->vgmem, lv))
#define _lv_modules_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(data_lv, lv_data_lv_dup(lv->vg->vgmem, lv))
#define _data_lv_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(metadata_lv, lv_metadata_lv_dup(lv->vg->vgmem, lv))
#define _metadata_lv_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(pool_lv, lv_pool_lv_dup(lv->vg->vgmem, lv))
#define _pool_lv_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(data_percent, _data_percent(lv))
#define _data_percent_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(metadata_percent, _metadata_percent(lv))
#define _metadata_percent_set prop_not_implemented_set
GET_LV_NUM_PROPERTY_FN(lv_metadata_size, lv_metadata_size(lv) * SECTOR_SIZE)
#define _lv_metadata_size_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_time, lv_time_dup(lv->vg->vgmem, lv))
#define _lv_time_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_host, lv_host_dup(lv->vg->vgmem, lv))
#define _lv_host_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_active, lv_active_dup(lv->vg->vgmem, lv))
#define _lv_active_set prop_not_implemented_set
GET_LV_STR_PROPERTY_FN(lv_profile, lv_profile_dup(lv->vg->vgmem, lv))
#define _lv_profile_set prop_not_implemented_set

/* VG */
GET_VG_STR_PROPERTY_FN(vg_fmt, vg_fmt_dup(vg))
#define _vg_fmt_set prop_not_implemented_set
GET_VG_STR_PROPERTY_FN(vg_uuid, vg_uuid_dup(vg))
#define _vg_uuid_set prop_not_implemented_set
GET_VG_STR_PROPERTY_FN(vg_name, vg_name_dup(vg))
#define _vg_name_set prop_not_implemented_set
GET_VG_STR_PROPERTY_FN(vg_attr, vg_attr_dup(vg->vgmem, vg))
#define _vg_attr_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_size, (SECTOR_SIZE * vg_size(vg)))
#define _vg_size_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_free, (SECTOR_SIZE * vg_free(vg)))
#define _vg_free_set prop_not_implemented_set
GET_VG_STR_PROPERTY_FN(vg_sysid, vg_system_id_dup(vg))
#define _vg_sysid_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_extent_size, (SECTOR_SIZE * vg->extent_size))
#define _vg_extent_size_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_extent_count, vg->extent_count)
#define _vg_extent_count_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_free_count, vg->free_count)
#define _vg_free_count_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(max_lv, vg->max_lv)
#define _max_lv_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(max_pv, vg->max_pv)
#define _max_pv_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(pv_count, vg->pv_count)
#define _pv_count_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(lv_count, (vg_visible_lvs(vg)))
#define _lv_count_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(snap_count, (snapshot_count(vg)))
#define _snap_count_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_seqno, vg->seqno)
#define _vg_seqno_set prop_not_implemented_set
GET_VG_STR_PROPERTY_FN(vg_tags, vg_tags_dup(vg))
#define _vg_tags_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_mda_count, (vg_mda_count(vg)))
#define _vg_mda_count_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_mda_used_count, (vg_mda_used_count(vg)))
#define _vg_mda_used_count_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_mda_free, (SECTOR_SIZE * vg_mda_free(vg)))
#define _vg_mda_free_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_mda_size, (SECTOR_SIZE * vg_mda_size(vg)))
#define _vg_mda_size_set prop_not_implemented_set
GET_VG_NUM_PROPERTY_FN(vg_mda_copies, (vg_mda_copies(vg)))
SET_VG_NUM_PROPERTY_FN(vg_mda_copies, vg_set_mda_copies)
GET_VG_STR_PROPERTY_FN(vg_profile, vg_profile_dup(vg))
#define _vg_profile_set prop_not_implemented_set

/* LVSEG */
GET_LVSEG_STR_PROPERTY_FN(segtype, lvseg_segtype_dup(lvseg->lv->vg->vgmem, lvseg))
#define _segtype_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(stripes, lvseg->area_count)
#define _stripes_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(stripesize, (SECTOR_SIZE * lvseg->stripe_size))
#define _stripesize_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(stripe_size, (SECTOR_SIZE * lvseg->stripe_size))
#define _stripe_size_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(regionsize, (SECTOR_SIZE * lvseg->region_size))
#define _regionsize_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(region_size, (SECTOR_SIZE * lvseg->region_size))
#define _region_size_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(chunksize, (SECTOR_SIZE * lvseg_chunksize(lvseg)))
#define _chunksize_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(chunk_size, (SECTOR_SIZE * lvseg_chunksize(lvseg)))
#define _chunk_size_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(thin_count, dm_list_size(&lvseg->lv->segs_using_this_lv))
#define _thin_count_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(zero, lvseg->zero_new_blocks)
#define _zero_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(transaction_id, lvseg->transaction_id)
#define _transaction_id_set prop_not_implemented_set
GET_LVSEG_STR_PROPERTY_FN(discards, lvseg_discards_dup(lvseg->lv->vg->vgmem, lvseg))
#define _discards_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(seg_start, (SECTOR_SIZE * lvseg_start(lvseg)))
#define _seg_start_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(seg_start_pe, lvseg->le)
#define _seg_start_pe_set prop_not_implemented_set
GET_LVSEG_NUM_PROPERTY_FN(seg_size, (SECTOR_SIZE * lvseg_size(lvseg)))
#define _seg_size_set prop_not_implemented_set
GET_LVSEG_STR_PROPERTY_FN(seg_tags, lvseg_tags_dup(lvseg))
#define _seg_tags_set prop_not_implemented_set
GET_LVSEG_STR_PROPERTY_FN(seg_pe_ranges,
			  lvseg_seg_pe_ranges(lvseg->lv->vg->vgmem, lvseg))
#define _seg_pe_ranges_set prop_not_implemented_set
GET_LVSEG_STR_PROPERTY_FN(devices, lvseg_devices(lvseg->lv->vg->vgmem, lvseg))
#define _devices_set prop_not_implemented_set
GET_LVSEG_STR_PROPERTY_FN(seg_monitor, lvseg_monitor_dup(lvseg->lv->vg->vgmem, lvseg))
#define _seg_monitor_set prop_not_implemented_set

/* PVSEG */
GET_PVSEG_NUM_PROPERTY_FN(pvseg_start, pvseg->pe)
#define _pvseg_start_set prop_not_implemented_set
GET_PVSEG_NUM_PROPERTY_FN(pvseg_size, (SECTOR_SIZE * pvseg->len))
#define _pvseg_size_set prop_not_implemented_set

struct lvm_property_type _properties[] = {
#include "columns.h"
	{ 0, "", 0, 0, 0, { .integer = 0 }, prop_not_implemented_get, prop_not_implemented_set },
};

#undef STR
#undef NUM
#undef FIELD

int lvseg_get_property(const struct lv_segment *lvseg,
		       struct lvm_property_type *prop)
{
	return prop_get_property(_properties, lvseg, prop, SEGS);
}

int lv_get_property(const struct logical_volume *lv,
		    struct lvm_property_type *prop)
{
	return prop_get_property(_properties, lv, prop, LVS);
}

int vg_get_property(const struct volume_group *vg,
		    struct lvm_property_type *prop)
{
	return prop_get_property(_properties, vg, prop, VGS);
}

int pvseg_get_property(const struct pv_segment *pvseg,
		       struct lvm_property_type *prop)
{
	return prop_get_property(_properties, pvseg, prop, PVSEGS);
}

int pv_get_property(const struct physical_volume *pv,
		    struct lvm_property_type *prop)
{
	return prop_get_property(_properties, pv, prop, PVS | LABEL);
}

int lv_set_property(struct logical_volume *lv,
		    struct lvm_property_type *prop)
{
	return prop_set_property(_properties, lv, prop, LVS);
}

int vg_set_property(struct volume_group *vg,
		    struct lvm_property_type *prop)
{
	return prop_set_property(_properties, vg, prop, VGS);
}

int pv_set_property(struct physical_volume *pv,
		    struct lvm_property_type *prop)
{
	return prop_set_property(_properties, pv, prop, PVS | LABEL);
}
