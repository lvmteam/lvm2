/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Report type, Containing struct, Field type, Report heading,
 * Data field with struct to pass to display function, Minimum display width,
 * Display Fn, Unique format identifier */

/* *INDENT-OFF* */
FIELD(LVS, lv, STR, "LV UUID", lvid.id[1], 38, uuid, "lv_uuid")
FIELD(LVS, lv, STR, "LV", name, 4, string, "lv_name")
FIELD(LVS, lv, STR, "Attr", lvid, 4, lvstatus, "lv_attr")
FIELD(LVS, lv, NUM, "Maj", major, 3, int32, "lv_major")
FIELD(LVS, lv, NUM, "Min", minor, 3, int32, "lv_minor")
FIELD(LVS, lv, STR, "KMaj", lvid, 4, lvkmaj, "lv_kernel_major")
FIELD(LVS, lv, STR, "KMin", lvid, 4, lvkmin, "lv_kernel_minor")
FIELD(LVS, lv, NUM, "LSize", size, 5, size64, "lv_size")
FIELD(LVS, lv, NUM, "#Seg", lvid, 4, lvsegcount, "seg_count")
FIELD(LVS, lv, STR, "Origin", lvid, 6, origin, "origin")
FIELD(LVS, lv, NUM, "Snap%", lvid, 6, snpercent, "snap_percent")
FIELD(LVS, lv, NUM, "Copy%", lvid, 6, copypercent, "copy_percent")
FIELD(LVS, lv, STR, "Move", lvid, 4, movepv, "move_pv")
FIELD(LVS, lv, STR, "LV Tags", tags, 7, tags, "lv_tags")
FIELD(LVS, lv, STR, "Log", lvid, 3, loglv, "mirror_log")

FIELD(PVS, pv, STR, "Fmt", id, 3, pvfmt, "pv_fmt")
FIELD(PVS, pv, STR, "PV UUID", id, 38, uuid, "pv_uuid")
FIELD(PVS, pv, NUM, "PSize", id, 5, pvsize, "pv_size")
FIELD(PVS, pv, NUM, "DevSize", dev, 7, devsize, "dev_size")
FIELD(PVS, pv, NUM, "PFree", id, 5, pvfree, "pv_free")
FIELD(PVS, pv, NUM, "Used", id, 4, pvused, "pv_used")
FIELD(PVS, pv, STR, "PV", dev, 10, dev_name, "pv_name")
FIELD(PVS, pv, STR, "Attr", status, 4, pvstatus, "pv_attr")
FIELD(PVS, pv, NUM, "PE", pe_count, 3, uint32, "pv_pe_count")
FIELD(PVS, pv, NUM, "Alloc", pe_alloc_count, 5, uint32, "pv_pe_alloc_count")
FIELD(PVS, pv, STR, "PV Tags", tags, 7, tags, "pv_tags")

FIELD(VGS, vg, STR, "Fmt", cmd, 3, vgfmt, "vg_fmt")
FIELD(VGS, vg, STR, "VG UUID", id, 38, uuid, "vg_uuid")
FIELD(VGS, vg, STR, "VG", name, 4, string, "vg_name")
FIELD(VGS, vg, STR, "Attr", cmd, 5, vgstatus, "vg_attr")
FIELD(VGS, vg, NUM, "VSize", cmd, 5, vgsize, "vg_size")
FIELD(VGS, vg, NUM, "VFree", cmd, 5, vgfree, "vg_free")
FIELD(VGS, vg, STR, "SYS ID", system_id, 6, string, "vg_sysid")
FIELD(VGS, vg, NUM, "Ext", extent_size, 3, size32, "vg_extent_size")
FIELD(VGS, vg, NUM, "#Ext", extent_count, 4, uint32, "vg_extent_count")
FIELD(VGS, vg, NUM, "Free", free_count, 4, uint32, "vg_free_count")
FIELD(VGS, vg, NUM, "MaxLV", max_lv, 5, uint32, "max_lv")
FIELD(VGS, vg, NUM, "MaxPV", max_pv, 5, uint32, "max_pv")
FIELD(VGS, vg, NUM, "#PV", pv_count, 3, uint32, "pv_count")
FIELD(VGS, vg, NUM, "#LV", lv_count, 3, uint32, "lv_count")
FIELD(VGS, vg, NUM, "#SN", snapshot_count, 3, uint32, "snap_count")
FIELD(VGS, vg, NUM, "Seq", seqno, 3, uint32, "vg_seqno")
FIELD(VGS, vg, STR, "VG Tags", tags, 7, tags, "vg_tags")

FIELD(SEGS, seg, STR, "Type", list, 4, segtype, "segtype")
FIELD(SEGS, seg, NUM, "#Str", area_count, 4, uint32, "stripes")
FIELD(SEGS, seg, NUM, "Stripe", stripe_size, 6, size32, "stripesize")
FIELD(SEGS, seg, NUM, "Chunk", chunk_size, 5, size32, "chunksize")
FIELD(SEGS, seg, NUM, "Region", region_size, 6, size32, "regionsize")
FIELD(SEGS, seg, NUM, "Start", list, 5, segstart, "seg_start")
FIELD(SEGS, seg, NUM, "SSize", list, 5, segsize, "seg_size")
FIELD(SEGS, seg, STR, "Seg Tags", tags, 8, tags, "seg_tags")
FIELD(SEGS, seg, STR, "Devices", list, 5, devices, "devices")

FIELD(PVSEGS, pvseg, NUM, "Start", pe, 5, uint32, "pvseg_start")
FIELD(PVSEGS, pvseg, NUM, "SSize", len, 5, uint32, "pvseg_size")
/* *INDENT-ON* */
