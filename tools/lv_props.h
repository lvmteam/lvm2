/*
 * Copyright (C) 2016-2024 Red Hat, Inc. All rights reserved.
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

/*
 * Update toollib.c:_lv_is_prop() when adding
 * new is_XXXXX_LVP test
 */

/* enum value 0 means none */
lvp(is_locked)
lvp(is_partial)
lvp(is_virtual)
lvp(is_merging)
lvp(is_merging_origin)
lvp(is_converting)
lvp(is_external_origin)
lvp(is_virtual_origin)
lvp(is_not_synced)
lvp(is_pending_delete)
lvp(is_error_when_full)
lvp(is_pvmove)
lvp(is_removed)
lvp(is_vg_writable)
lvp(is_writable)

/* kinds of sub LV */
lvp(is_thinpool_data)
lvp(is_thinpool_metadata)
lvp(is_cachepool_data)
lvp(is_cachepool_metadata)
lvp(is_mirror_image)
lvp(is_mirror_log)
lvp(is_raid_image)
lvp(is_raid_metadata)

/*
 * is_thick_origin should be used instead of is_origin
 * is_thick_snapshot is generally used as LV_snapshot from lv_types.h
 */
lvp(is_origin)
lvp(is_thick_origin)
lvp(is_thick_snapshot)
lvp(is_thin_origin)
lvp(is_thin_snapshot)

lvp(is_error)
lvp(is_zero)

lvp(is_cache_origin)
lvp(is_cow)
lvp(is_merging_cow)
lvp(is_cow_covering_origin)
lvp(is_visible)
lvp(is_historical)
lvp(is_raid_with_tracking)
lvp(is_raid_with_integrity)
