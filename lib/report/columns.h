/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
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

/*
 * This file defines the fields (columns) for the reporting commands
 * (pvs/vgs/lvs).
 *
 * The preferred order of the field descriptions in the help text
 * determines the order the entries appear in this file.
 *
 * When adding new entries take care to use the existing style.
 * Displayed fields names normally have a type prefix and use underscores.
 * Field-specific internal functions names normally match the displayed
 * field names but without underscores.
 * Help text ends with a full stop.
 */

/*
 * FIELD(report_object_type, structure, sort_type, heading, structure_field, output width, reporting_function, field_id, description, settable_via_lib)
 */

/* *INDENT-OFF* */
FIELD(LVS, lv, STR, "LV UUID", lvid.id[1], 38, uuid, lv_uuid, "Unique identifier.", 0)
FIELD(LVS, lv, STR, "LV", lvid, 4, lvname, lv_name, "Name.  LVs created for internal use are enclosed in brackets.", 0)
FIELD(LVS, lv, STR, "LV", lvid, 4, lvfullname, lv_full_name, "Full name of LV including its VG, namely VG/LV.", 0)
FIELD(LVS, lv, STR, "Path", lvid, 4, lvpath, lv_path, "Full pathname for LV. Blank for internal LVs.", 0)
FIELD(LVS, lv, STR, "DMPath", lvid, 6, lvdmpath, lv_dm_path, "Internal device-mapper pathname for LV (in /dev/mapper directory).", 0)
FIELD(LVS, lv, STR, "Parent", lvid, 6, lvparent, lv_parent, "For LVs that are components of another LV, the parent LV.", 0)
FIELD(LVSINFOSTATUS, lv, STR, "Attr", lvid, 4, lvstatus, lv_attr, "Various attributes - see man page.", 0)
FIELD(LVS, lv, STR_LIST, "Layout", lvid, 10, lvlayout, lv_layout, "LV layout.", 0)
FIELD(LVS, lv, STR_LIST, "Role", lvid, 10, lvrole, lv_role, "LV role.", 0)
FIELD(LVS, lv, BIN, "InitImgSync", lvid, 10, lvinitialimagesync, lv_initial_image_sync, "Set if mirror/RAID images underwent initial resynchronization.", 0)
FIELD(LVS, lv, BIN, "ImgSynced", lvid, 10, lvimagesynced, lv_image_synced, "Set if mirror/RAID image is synchronized.", 0)
FIELD(LVS, lv, BIN, "Merging", lvid, 10, lvmerging, lv_merging, "Set if snapshot LV is being merged to origin.", 0)
FIELD(LVS, lv, BIN, "Converting", lvid, 10, lvconverting, lv_converting, "Set if LV is being converted.", 0)
FIELD(LVS, lv, STR, "AllocPol", lvid, 10, lvallocationpolicy, lv_allocation_policy, "LV allocation policy.", 0)
FIELD(LVS, lv, BIN, "AllocLock", lvid, 10, lvallocationlocked, lv_allocation_locked, "Set if LV is locked against allocation changes.", 0)
FIELD(LVS, lv, BIN, "FixMin", lvid, 10, lvfixedminor, lv_fixed_minor, "Set if LV has fixed minor number assigned.", 0)
FIELD(LVS, lv, BIN, "MergeFailed", lvid, 15, lvmergefailed, lv_merge_failed, "Set if snapshot merge failed.", 0)
FIELD(LVS, lv, BIN, "SnapInvalid", lvid, 15, lvsnapshotinvalid, lv_snapshot_invalid, "Set if snapshot LV is invalid.", 0)
FIELD(LVS, lv, BIN, "SkipAct", lvid, 15, lvskipactivation, lv_skip_activation, "Set if LV is skipped on activation.", 0)
FIELD(LVS, lv, STR, "WhenFull", lvid, 15, lvwhenfull, lv_when_full, "For thin pools, behavior when full.", 0)
FIELD(LVS, lv, STR, "Active", lvid, 6, lvactive, lv_active, "Active state of the LV.", 0)
FIELD(LVS, lv, BIN, "ActLocal", lvid, 10, lvactivelocally, lv_active_locally, "Set if the LV is active locally.", 0)
FIELD(LVS, lv, BIN, "ActRemote", lvid, 10, lvactiveremotely, lv_active_remotely, "Set if the LV is active remotely.", 0)
FIELD(LVS, lv, BIN, "ActExcl", lvid, 10, lvactiveexclusively, lv_active_exclusively, "Set if the LV is active exclusively.", 0)
FIELD(LVS, lv, SNUM, "Maj", major, 3, int32, lv_major, "Persistent major number or -1 if not persistent.", 0)
FIELD(LVS, lv, SNUM, "Min", minor, 3, int32, lv_minor, "Persistent minor number or -1 if not persistent.", 0)
FIELD(LVS, lv, SIZ, "Rahead", lvid, 6, lvreadahead, lv_read_ahead, "Read ahead setting in current units.", 0)
FIELD(LVS, lv, SIZ, "LSize", size, 5, size64, lv_size, "Size of LV in current units.", 0)
FIELD(LVS, lv, SIZ, "MSize", lvid, 6, lvmetadatasize, lv_metadata_size, "For thin and cache pools, the size of the LV that holds the metadata.", 0)
FIELD(LVS, lv, NUM, "#Seg", lvid, 4, lvsegcount, seg_count, "Number of segments in LV.", 0)
FIELD(LVS, lv, STR, "Origin", lvid, 6, origin, origin, "For snapshots, the origin device of this LV.", 0)
FIELD(LVS, lv, SIZ, "OSize", lvid, 5, originsize, origin_size, "For snapshots, the size of the origin device of this LV.", 0)
FIELD(LVS, lv, STR_LIST, "Ancestors", lvid, 12, lvancestors, lv_ancestors, "Ancestors of this LV.", 0)
FIELD(LVS, lv, STR_LIST, "Descendants", lvid, 12, lvdescendants, lv_descendants, "Descendants of this LV.", 0)
FIELD(LVS, lv, PCT, "Data%", lvid, 6, datapercent, data_percent, "For snapshot and thin pools and volumes, the percentage full if LV is active.", 0)
FIELD(LVS, lv, PCT, "Snap%", lvid, 6, snpercent, snap_percent, "For snapshots, the percentage full if LV is active.", 0)
FIELD(LVS, lv, PCT, "Meta%", lvid, 6, metadatapercent, metadata_percent, "For thin pools, the percentage of metadata full if LV is active.", 0)
FIELD(LVS, lv, PCT, "Cpy%Sync", lvid, 8, copypercent, copy_percent, "For RAID, mirrors and pvmove, current percentage in-sync.", 0)
FIELD(LVS, lv, PCT, "Cpy%Sync", lvid, 8, copypercent, sync_percent, "For RAID, mirrors and pvmove, current percentage in-sync.", 0)
FIELD(LVS, lv, NUM, "Mismatches", lvid, 10, raidmismatchcount, raid_mismatch_count, "For RAID, number of mismatches found or repaired.", 0)
FIELD(LVS, lv, STR, "SyncAction", lvid, 10, raidsyncaction, raid_sync_action, "For RAID, the current synchronization action being performed.", 0)
FIELD(LVS, lv, NUM, "WBehind", lvid, 7, raidwritebehind, raid_write_behind, "For RAID1, the number of outstanding writes allowed to writemostly devices.", 0)
FIELD(LVS, lv, NUM, "MinSync", lvid, 7, raidminrecoveryrate, raid_min_recovery_rate, "For RAID1, the minimum recovery I/O load in kiB/sec/disk.", 0)
FIELD(LVS, lv, NUM, "MaxSync", lvid, 7, raidmaxrecoveryrate, raid_max_recovery_rate, "For RAID1, the maximum recovery I/O load in kiB/sec/disk.", 0)
FIELD(LVS, lv, STR, "Move", lvid, 4, movepv, move_pv, "For pvmove, Source PV of temporary LV created by pvmove.", 0)
FIELD(LVS, lv, STR, "Convert", lvid, 7, convertlv, convert_lv, "For lvconvert, Name of temporary LV created by lvconvert.", 0)
FIELD(LVS, lv, STR, "Log", lvid, 3, loglv, mirror_log, "For mirrors, the LV holding the synchronisation log.", 0)
FIELD(LVS, lv, STR, "Data", lvid, 4, datalv, data_lv, "For thin and cache pools, the LV holding the associated data.", 0)
FIELD(LVS, lv, STR, "Meta", lvid, 4, metadatalv, metadata_lv, "For thin and cache pools, the LV holding the associated metadata.", 0)
FIELD(LVS, lv, STR, "Pool", lvid, 4, poollv, pool_lv, "For thin volumes, the thin pool LV for this volume.", 0)
FIELD(LVS, lv, STR_LIST, "LV Tags", tags, 7, tags, lv_tags, "Tags, if any.", 0)
FIELD(LVS, lv, STR, "LProfile", lvid, 8, lvprofile, lv_profile, "Configuration profile attached to this LV.", 0)
FIELD(LVS, lv, STR, "Time", lvid, 26, lvtime, lv_time, "Creation time of the LV, if known", 0)
FIELD(LVS, lv, STR, "Host", lvid, 10, lvhost, lv_host, "Creation host of the LV, if known.", 0)
FIELD(LVS, lv, STR_LIST, "Modules", lvid, 7, modules, lv_modules, "Kernel device-mapper modules required for this LV.", 0)

FIELD(LVSINFO, lv, SNUM, "KMaj", lvid, 4, lvkmaj, lv_kernel_major, "Currently assigned major number or -1 if LV is not active.", 0)
FIELD(LVSINFO, lv, SNUM, "KMin", lvid, 4, lvkmin, lv_kernel_minor, "Currently assigned minor number or -1 if LV is not active.", 0)
FIELD(LVSINFO, lv, SIZ, "KRahead", lvid, 7, lvkreadahead, lv_kernel_read_ahead, "Currently-in-use read ahead setting in current units.", 0)
FIELD(LVSINFO, lv, STR, "LPerms", lvid, 8, lvpermissions, lv_permissions, "LV permissions.", 0)
FIELD(LVSINFO, lv, BIN, "Suspended", lvid, 10, lvsuspended, lv_suspended, "Set if LV is suspended.", 0)
FIELD(LVSINFO, lv, BIN, "LiveTable", lvid, 20, lvlivetable, lv_live_table, "Set if LV has live table present.", 0)
FIELD(LVSINFO, lv, BIN, "InactiveTable", lvid, 20, lvinactivetable, lv_inactive_table, "Set if LV has inactive table present.", 0)
FIELD(LVSINFO, lv, BIN, "DevOpen", lvid, 10, lvdeviceopen, lv_device_open, "Set if LV device is open.", 0)

FIELD(LVSSTATUS, lv, NUM, "CacheTotalBlocks", lvid, 16, cache_total_blocks, cache_total_blocks, "Total cache blocks.", 0)
FIELD(LVSSTATUS, lv, NUM, "CacheUsedBlocks", lvid, 16, cache_used_blocks, cache_used_blocks, "Used cache blocks.", 0)
FIELD(LVSSTATUS, lv, NUM, "CacheDirtyBlocks", lvid, 16, cache_dirty_blocks, cache_dirty_blocks, "Dirty cache blocks.", 0)
FIELD(LVSSTATUS, lv, NUM, "CacheReadHits", lvid, 16, cache_read_hits, cache_read_hits, "Cache read hits.", 0)
FIELD(LVSSTATUS, lv, NUM, "CacheReadMisses", lvid, 16, cache_read_misses, cache_read_misses, "Cache read misses.", 0)
FIELD(LVSSTATUS, lv, NUM, "CacheWriteHits", lvid, 16, cache_write_hits, cache_write_hits, "Cache write hits.", 0)
FIELD(LVSSTATUS, lv, NUM, "CacheWriteMisses", lvid, 16, cache_write_misses, cache_write_misses, "Cache write misses.", 0)
FIELD(LVSSTATUS, lv, STR, "Health", lvid, 15, lvhealthstatus, lv_health_status, "LV health status.", 0)

FIELD(LABEL, label, STR, "Fmt", type, 3, pvfmt, pv_fmt, "Type of metadata.", 0)
FIELD(LABEL, label, STR, "PV UUID", type, 38, pvuuid, pv_uuid, "Unique identifier.", 0)
FIELD(LABEL, label, SIZ, "DevSize", dev, 7, devsize, dev_size, "Size of underlying device in current units.", 0)
FIELD(LABEL, label, STR, "PV", dev, 10, dev_name, pv_name, "Name.", 0)
FIELD(LABEL, label, SIZ, "PMdaFree", type, 9, pvmdafree, pv_mda_free, "Free metadata area space on this device in current units.", 0)
FIELD(LABEL, label, SIZ, "PMdaSize", type, 9, pvmdasize, pv_mda_size, "Size of smallest metadata area on this device in current units.", 0)

FIELD(PVS, pv, NUM, "1st PE", pe_start, 7, size64, pe_start, "Offset to the start of data on the underlying device.", 0)
FIELD(PVS, pv, SIZ, "PSize", id, 5, pvsize, pv_size, "Size of PV in current units.", 0)
FIELD(PVS, pv, SIZ, "PFree", id, 5, pvfree, pv_free, "Total amount of unallocated space in current units.", 0)
FIELD(PVS, pv, SIZ, "Used", id, 4, pvused, pv_used, "Total amount of allocated space in current units.", 0)
FIELD(PVS, pv, STR, "Attr", id, 4, pvstatus, pv_attr, "Various attributes - see man page.", 0)
FIELD(PVS, pv, BIN, "Allocatable", id, 10, pvallocatable, pv_allocatable, "Set if this device can be used for allocation.", 0)
FIELD(PVS, pv, BIN, "Exported", id, 10, pvexported, pv_exported, "Set if this device is exported.", 0)
FIELD(PVS, pv, BIN, "Missing", id, 10, pvmissing, pv_missing, "Set if this device is missing in system.", 0)
FIELD(PVS, pv, NUM, "PE", pe_count, 3, uint32, pv_pe_count, "Total number of Physical Extents.", 0)
FIELD(PVS, pv, NUM, "Alloc", pe_alloc_count, 5, uint32, pv_pe_alloc_count, "Total number of allocated Physical Extents.", 0)
FIELD(PVS, pv, STR_LIST, "PV Tags", tags, 7, tags, pv_tags, "Tags, if any.", 0)
FIELD(PVS, pv, NUM, "#PMda", id, 5, pvmdas, pv_mda_count, "Number of metadata areas on this device.", 0)
FIELD(PVS, pv, NUM, "#PMdaUse", id, 8, pvmdasused, pv_mda_used_count, "Number of metadata areas in use on this device.", 0)
FIELD(PVS, pv, SIZ, "BA start", ba_start, 8, size64, pv_ba_start, "Offset to the start of PV Bootloader Area on the underlying device in current units.", 0)
FIELD(PVS, pv, SIZ, "BA size", ba_size, 7, size64, pv_ba_size, "Size of PV Bootloader Area in current units.", 0)

FIELD(VGS, vg, STR, "Fmt", cmd, 3, vgfmt, vg_fmt, "Type of metadata.", 0)
FIELD(VGS, vg, STR, "VG UUID", id, 38, uuid, vg_uuid, "Unique identifier.", 0)
FIELD(VGS, vg, STR, "VG", name, 4, string, vg_name, "Name.", 0)
FIELD(VGS, vg, STR, "Attr", cmd, 5, vgstatus, vg_attr, "Various attributes - see man page.", 0)
FIELD(VGS, vg, STR, "VPerms", cmd, 10, vgpermissions, vg_permissions, "VG permissions.", 0)
FIELD(VGS, vg, BIN, "Extendable", cmd, 10, vgextendable, vg_extendable, "Set if VG is extendable.", 0)
FIELD(VGS, vg, BIN, "Exported", cmd, 10, vgexported, vg_exported, "Set if VG is exported.", 0)
FIELD(VGS, vg, BIN, "Partial", cmd, 10, vgpartial, vg_partial, "Set if VG is partial.", 0)
FIELD(VGS, vg, STR, "AllocPol", cmd, 10, vgallocationpolicy, vg_allocation_policy, "VG allocation policy.", 0)
FIELD(VGS, vg, BIN, "Clustered", cmd, 10, vgclustered, vg_clustered, "Set if VG is clustered.", 0)
FIELD(VGS, vg, SIZ, "VSize", cmd, 5, vgsize, vg_size, "Total size of VG in current units.", 0)
FIELD(VGS, vg, SIZ, "VFree", cmd, 5, vgfree, vg_free, "Total amount of free space in current units.", 0)
FIELD(VGS, vg, STR, "SYS ID", cmd, 6, vgsystemid, vg_sysid, "System ID of the VG indicating which host owns it.", 0)
FIELD(VGS, vg, STR, "System ID", cmd, 9, vgsystemid, vg_systemid, "System ID of the VG indicating which host owns it.", 0)
FIELD(VGS, vg, SIZ, "Ext", extent_size, 3, size32, vg_extent_size, "Size of Physical Extents in current units.", 0)
FIELD(VGS, vg, NUM, "#Ext", extent_count, 4, uint32, vg_extent_count, "Total number of Physical Extents.", 0)
FIELD(VGS, vg, NUM, "Free", free_count, 4, uint32, vg_free_count, "Total number of unallocated Physical Extents.", 0)
FIELD(VGS, vg, NUM, "MaxLV", max_lv, 5, uint32, max_lv, "Maximum number of LVs allowed in VG or 0 if unlimited.", 0)
FIELD(VGS, vg, NUM, "MaxPV", max_pv, 5, uint32, max_pv, "Maximum number of PVs allowed in VG or 0 if unlimited.", 0)
FIELD(VGS, vg, NUM, "#PV", pv_count, 3, uint32, pv_count, "Number of PVs.", 0)
FIELD(VGS, vg, NUM, "#LV", cmd, 3, lvcount, lv_count, "Number of LVs.", 0)
FIELD(VGS, vg, NUM, "#SN", cmd, 3, snapcount, snap_count, "Number of snapshots.", 0)
FIELD(VGS, vg, NUM, "Seq", seqno, 3, uint32, vg_seqno, "Revision number of internal metadata.  Incremented whenever it changes.", 0)
FIELD(VGS, vg, STR_LIST, "VG Tags", tags, 7, tags, vg_tags, "Tags, if any.", 0)
FIELD(VGS, vg, STR, "VProfile", cmd, 8, vgprofile, vg_profile, "Configuration profile attached to this VG.", 0)
FIELD(VGS, vg, NUM, "#VMda", cmd, 5, vgmdas, vg_mda_count, "Number of metadata areas on this VG.", 0)
FIELD(VGS, vg, NUM, "#VMdaUse", cmd, 8, vgmdasused, vg_mda_used_count, "Number of metadata areas in use on this VG.", 0)
FIELD(VGS, vg, SIZ, "VMdaFree", cmd, 9, vgmdafree, vg_mda_free, "Free metadata area space for this VG in current units.", 0)
FIELD(VGS, vg, SIZ, "VMdaSize", cmd, 9, vgmdasize, vg_mda_size, "Size of smallest metadata area for this VG in current units.", 0)
FIELD(VGS, vg, NUM, "#VMdaCps", cmd, 8, vgmdacopies, vg_mda_copies, "Target number of in use metadata areas in the VG.", 1)

FIELD(SEGS, seg, STR, "Type", list, 4, segtype, segtype, "Type of LV segment.", 0)
FIELD(SEGS, seg, NUM, "#Str", area_count, 4, uint32, stripes, "Number of stripes or mirror legs.", 0)
FIELD(SEGS, seg, SIZ, "Stripe", stripe_size, 6, size32, stripesize, "For stripes, amount of data placed on one device before switching to the next.", 0)
FIELD(SEGS, seg, SIZ, "Stripe", stripe_size, 6, size32, stripe_size, "For stripes, amount of data placed on one device before switching to the next.", 0)
FIELD(SEGS, seg, SIZ, "Region", region_size, 6, size32, regionsize, "For mirrors, the unit of data copied when synchronising devices.", 0)
FIELD(SEGS, seg, SIZ, "Region", region_size, 6, size32, region_size, "For mirrors, the unit of data copied when synchronising devices.", 0)
FIELD(SEGS, seg, SIZ, "Chunk", list, 5, chunksize, chunksize, "For snapshots, the unit of data used when tracking changes.", 0)
FIELD(SEGS, seg, SIZ, "Chunk", list, 5, chunksize, chunk_size, "For snapshots, the unit of data used when tracking changes.", 0)
FIELD(SEGS, seg, NUM, "#Thins", list, 4, thincount, thin_count, "For thin pools, the number of thin volumes in this pool.", 0)
FIELD(SEGS, seg, STR, "Discards", list, 8, discards, discards, "For thin pools, how discards are handled.", 0)
FIELD(SEGS, seg, STR, "Cachemode", list, 9, cachemode, cachemode, "For cache pools, how writes are cached.", 0)
FIELD(SEGS, seg, BIN, "Zero", list, 4, thinzero, zero, "For thin pools, if zeroing is enabled.", 0)
FIELD(SEGS, seg, NUM, "TransId", list, 4, transactionid, transaction_id, "For thin pools, the transaction id.", 0)
FIELD(SEGS, seg, NUM, "ThId", list, 4, thinid, thin_id, "For thin volume, the thin device id.", 0)
FIELD(SEGS, seg, SIZ, "Start", list, 5, segstart, seg_start, "Offset within the LV to the start of the segment in current units.", 0)
FIELD(SEGS, seg, NUM, "Start", list, 5, segstartpe, seg_start_pe, "Offset within the LV to the start of the segment in physical extents.", 0)
FIELD(SEGS, seg, SIZ, "SSize", list, 5, segsize, seg_size, "Size of segment in current units.", 0)
FIELD(SEGS, seg, SIZ, "SSize", list, 5, segsizepe, seg_size_pe, "Size of segment in physical extents.", 0)
FIELD(SEGS, seg, STR_LIST, "Seg Tags", tags, 8, tags, seg_tags, "Tags, if any.", 0)
FIELD(SEGS, seg, STR, "PE Ranges", list, 9, peranges, seg_pe_ranges, "Ranges of Physical Extents of underlying devices in command line format.", 0)
FIELD(SEGS, seg, STR, "Devices", list, 7, devices, devices, "Underlying devices used with starting extent numbers.", 0)
FIELD(SEGS, seg, STR, "Monitor", list, 7, segmonitor, seg_monitor, "Dmeventd monitoring status of the segment.", 0)
FIELD(SEGS, seg, STR, "Cache Policy", list, 12, cache_policy, cache_policy, "The cache policy (cached segments only).", 0)
FIELD(SEGS, seg, STR_LIST, "Cache Settings", list, 14, cache_settings, cache_settings, "Cache settings/parameters (cached segments only).", 0)

FIELD(PVSEGS, pvseg, NUM, "Start", pe, 5, uint32, pvseg_start, "Physical Extent number of start of segment.", 0)
FIELD(PVSEGS, pvseg, NUM, "SSize", len, 5, uint32, pvseg_size, "Number of extents in segment.", 0)
/* *INDENT-ON* */
