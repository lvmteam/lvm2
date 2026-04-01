#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test report output sorting with -O for all field types:
#   STR        (lv_name, lv_attr)
#   SIZ        (lv_size)
#   NUM        (seg_count)
#   STR_LIST   (lv_tags, lv_layout, lv_role)
# Also test multi-key sorting and ascending/descending directions.

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux prepare_vg 3

# Use --rows with --separator to get all sorted values on a single
# line in a predictable format: val1:val2:val3
# --noheadings suppresses the field heading prefix.
# --separator disables column alignment/padding.
ROPTS="--noheadings --rows --separator :"

#
# Create LVs with different sizes and names so we can test sorting
# by STR (lv_name), NUM (seg_count), SIZ (lv_size), and STR_LIST (lv_tags).
#
lvcreate -an -l 8 -n cc_lv $vg
lvcreate -an -l 4 -n aa_lv $vg
lvcreate -an -l 12 -n bb_lv $vg

# Add tags (STR_LIST field) to each LV to test string list sorting.
# Tags are displayed sorted within each field value, comma-separated.
lvchange --addtag ztag --addtag atag $vg/cc_lv
lvchange --addtag mtag $vg/aa_lv
lvchange --addtag btag --addtag ctag $vg/bb_lv

# -------------------------------------------------------------------
# 1. Sort by STR field (lv_name) - ascending (default)
# -------------------------------------------------------------------
lvs $ROPTS -o lv_name -O lv_name $vg > out
grep "aa_lv:bb_lv:cc_lv" out

# Sort by STR field (lv_name) - explicit ascending
lvs $ROPTS -o lv_name -O +lv_name $vg > out
grep "aa_lv:bb_lv:cc_lv" out

# Sort by STR field (lv_name) - descending
lvs $ROPTS -o lv_name -O -lv_name $vg > out
grep "cc_lv:bb_lv:aa_lv" out

# -------------------------------------------------------------------
# 2. Sort by SIZ field (lv_size) - ascending
#    4 extents (aa_lv) < 8 extents (cc_lv) < 12 extents (bb_lv)
# -------------------------------------------------------------------
lvs $ROPTS -o lv_name -O lv_size $vg > out
grep "aa_lv:cc_lv:bb_lv" out

# Sort by SIZ field (lv_size) - descending
lvs $ROPTS -o lv_name -O -lv_size $vg > out
grep "bb_lv:cc_lv:aa_lv" out

# -------------------------------------------------------------------
# 3. Sort by NUM field (seg_count) - ascending
#    All LVs have 1 segment - order is unspecified but all must appear.
# -------------------------------------------------------------------
lvs $ROPTS -o lv_name -O seg_count $vg > out
grep "aa_lv" out | grep "bb_lv" | grep "cc_lv"

# -------------------------------------------------------------------
# 4. Sort by STR_LIST field (lv_tags) - ascending
#    Tag values (sorted within each LV):
#      cc_lv -> "atag,ztag"
#      aa_lv -> "mtag"
#      bb_lv -> "btag,ctag"
#    Ascending string order: "atag,ztag" < "btag,ctag" < "mtag"
# -------------------------------------------------------------------
lvs $ROPTS -o lv_name -O lv_tags $vg > out
grep "cc_lv:bb_lv:aa_lv" out

# Sort by STR_LIST field (lv_tags) - descending
lvs $ROPTS -o lv_name -O -lv_tags $vg > out
grep "aa_lv:bb_lv:cc_lv" out

# Verify the tags themselves are reported correctly when sorted
lvs $ROPTS -o lv_tags -O lv_tags $vg > out
grep "atag,ztag:btag,ctag:mtag" out

# -------------------------------------------------------------------
# 5. Sort by STR_LIST field with empty vs non-empty tags
#    Empty string sorts before any non-empty string.
# -------------------------------------------------------------------
lvcreate -an -l 4 -n dd_lv $vg
# dd_lv has no tags (empty string list)

lvs $ROPTS -o lv_name -O lv_tags $vg > out
# Empty tags sorts first in ascending order
grep "dd_lv:cc_lv:bb_lv:aa_lv" out

# Descending - empty tags last
lvs $ROPTS -o lv_name -O -lv_tags $vg > out
grep "aa_lv:bb_lv:cc_lv:dd_lv" out

lvremove -f $vg/dd_lv

# -------------------------------------------------------------------
# 6. Multi-key sort: primary STR_LIST (lv_tags), secondary STR (lv_name)
#    Give two LVs the same tags to test secondary sort key.
# -------------------------------------------------------------------
lvcreate -an -l 4 -n zz_lv $vg
lvchange --addtag mtag $vg/zz_lv
# Now aa_lv and zz_lv both have "mtag"

# Primary: lv_tags ascending, secondary: lv_name ascending
lvs $ROPTS -o lv_name -O lv_tags,lv_name $vg > out
# "atag,ztag" (cc_lv), "btag,ctag" (bb_lv), "mtag" (aa_lv before zz_lv)
grep "cc_lv:bb_lv:aa_lv:zz_lv" out

# Primary: lv_tags ascending, secondary: lv_name descending
lvs $ROPTS -o lv_name -O lv_tags,-lv_name $vg > out
grep "cc_lv:bb_lv:zz_lv:aa_lv" out

lvremove -f $vg/zz_lv

# -------------------------------------------------------------------
# 7. Multi-key sort: primary SIZ, secondary STR
# -------------------------------------------------------------------
lvcreate -an -l 4 -n ee_lv $vg
# Now aa_lv and ee_lv both have size 4 extents

# Primary: lv_size ascending, secondary: lv_name ascending
lvs $ROPTS -o lv_name -O lv_size,lv_name $vg > out
# size 4: aa_lv < ee_lv, then size 8: cc_lv, then size 12: bb_lv
grep "aa_lv:ee_lv:cc_lv:bb_lv" out

# Primary: lv_size ascending, secondary: lv_name descending
lvs $ROPTS -o lv_name -O lv_size,-lv_name $vg > out
grep "ee_lv:aa_lv:cc_lv:bb_lv" out

lvremove -f $vg/ee_lv

# -------------------------------------------------------------------
# 8. Sort by lv_attr (STR field with fixed-width encoded attributes)
#    All LVs have similar attributes - order is unspecified but all
#    must appear.
# -------------------------------------------------------------------
lvs $ROPTS -o lv_name -O lv_attr $vg > out
grep "aa_lv" out | grep "bb_lv" | grep "cc_lv"

# -------------------------------------------------------------------
# 9. Sort by STR_LIST field (lv_layout)
#    All simple linear LVs have layout "linear" - this tests that
#    sorting by a non-tag STR_LIST field works without error.
# -------------------------------------------------------------------
lvs $ROPTS -o lv_name -O lv_layout $vg > out
grep "aa_lv" out | grep "bb_lv" | grep "cc_lv"

# -------------------------------------------------------------------
# 10. Sort by STR_LIST field (lv_role)
# -------------------------------------------------------------------
lvs $ROPTS -o lv_name -O lv_role $vg > out
grep "aa_lv" out | grep "bb_lv" | grep "cc_lv"

# -------------------------------------------------------------------
# 11. Sort by NUM field (seg_count) with distinct values
#     Create LVs with 1, 2, and 3 segments by extending across PVs.
# -------------------------------------------------------------------
lvcreate -an -l 4 -n num1_lv $vg "$dev1"
lvcreate -an -l 4 -n num3_lv $vg "$dev1"
lvextend -l +4 $vg/num3_lv "$dev2"
lvextend -l +4 $vg/num3_lv "$dev3"
lvcreate -an -l 4 -n num2_lv $vg "$dev1"
lvextend -l +4 $vg/num2_lv "$dev2"
# num1_lv: 1 seg, num2_lv: 2 segs, num3_lv: 3 segs

lvs $ROPTS -o lv_name -O seg_count -S "lv_name=~^num" $vg > out
grep "num1_lv:num2_lv:num3_lv" out

lvs $ROPTS -o lv_name -O -seg_count -S "lv_name=~^num" $vg > out
grep "num3_lv:num2_lv:num1_lv" out

lvremove -f $vg/num1_lv $vg/num2_lv $vg/num3_lv

# -------------------------------------------------------------------
# 12. Sort by BIN field (lv_active)
#     BIN maps to DM_REPORT_FIELD_TYPE_NUMBER internally.
# -------------------------------------------------------------------
lvcreate -an -l 4 -n bin0_lv $vg
lvcreate -l 4 -n bin1_lv $vg

# Ascending: inactive (0) before active (1)
lvs $ROPTS -o lv_name -O lv_active -S "lv_name=~^bin" $vg > out
grep "bin0_lv:bin1_lv" out

# Descending: active (1) before inactive (0)
lvs $ROPTS -o lv_name -O -lv_active -S "lv_name=~^bin" $vg > out
grep "bin1_lv:bin0_lv" out

lvremove -f $vg/bin0_lv $vg/bin1_lv

# -------------------------------------------------------------------
# 13. Sort by TIM field (lv_time)
#     LV creation time has 1-second precision, use sleep to ensure
#     distinct timestamps.
# -------------------------------------------------------------------
lvcreate -an -l 4 -n tim1_lv $vg
sleep 1
lvcreate -an -l 4 -n tim2_lv $vg

# Ascending: earlier creation time first
lvs $ROPTS -o lv_name -O lv_time -S "lv_name=~^tim" $vg > out
grep "tim1_lv:tim2_lv" out

# Descending: later creation time first
lvs $ROPTS -o lv_name -O -lv_time -S "lv_name=~^tim" $vg > out
grep "tim2_lv:tim1_lv" out

lvremove -f $vg/tim1_lv $vg/tim2_lv

# -------------------------------------------------------------------
# 14. Sort by PCT field (snap_percent / data_percent)
#     PCT maps to DM_REPORT_FIELD_TYPE_PERCENT.
#     Create snapshots with different usage to get distinct values.
#     Non-snapshot LVs have DM_PERCENT_INVALID (empty display).
# -------------------------------------------------------------------
lvcreate -l 8 -n pct_orig $vg
lvcreate -l 4 -s -n pct_snap1 $vg/pct_orig
lvcreate -l 8 -s -n pct_snap2 $vg/pct_orig
# Write some data to origin so snapshots diverge
dd if=/dev/zero of="$DM_DEV_DIR/$vg/pct_orig" bs=4096 count=64 conv=fdatasync

# pct_snap1 (smaller COW) should have higher snap_percent than pct_snap2
# Ascending: lower percent first
lvs $ROPTS -o lv_name -O snap_percent -S "lv_name=~^pct_snap" $vg > out
grep "pct_snap2:pct_snap1" out

# Descending: higher percent first
lvs $ROPTS -o lv_name -O -snap_percent -S "lv_name=~^pct_snap" $vg > out
grep "pct_snap1:pct_snap2" out

lvremove -f $vg/pct_snap1 $vg/pct_snap2 $vg/pct_orig

vgremove -ff $vg
