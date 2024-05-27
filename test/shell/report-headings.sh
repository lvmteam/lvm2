#!/usr/bin/env bash

# Copyright (C) 2023 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_pvs 1

aux lvmconf 'log/prefix=""' \
	    'report/compact_output=0' \
	    'report/compact_output_cols=""'

OPTS="ba_start,seg_start,seg_start_pe"

aux lvmconf 'report/headings=0'

pvs -o $OPTS > out
not grep "BA Start Start Start" out
not grep "pv_ba_start seg_start seg_start_pe" out


aux lvmconf 'report/headings=1'

pvs -o $OPTS > out
grep "BA Start Start Start" out

pvs --headings 1 -o $OPTS > out
grep "BA Start Start Start" out

pvs --headings abbrev -o $OPTS > out
grep "BA Start Start Start" out

pvs --headings 2 -o $OPTS > out
grep "pv_ba_start seg_start seg_start_pe" out

pvs --headings full -o $OPTS > out
grep "pv_ba_start seg_start seg_start_pe" out

pvs --headings none -o $OPTS > out
not grep "BA Start Start Start" out
not grep "pv_ba_start seg_start seg_start_pe" out

pvs --rows -o $OPTS > out
grep "BA Start 0" out
grep "Start 0" out

pvs --rows --headings 1 -o $OPTS > out
grep "BA Start" out
grep "Start 0" out;

pvs --rows --headings abbrev -o $OPTS > out
grep "BA Start" out
grep "Start 0" out;

pvs --rows --headings 2 -o $OPTS > out
grep "pv_ba_start 0" out
grep "seg_start 0" out
grep "seg_start_pe 0" out

pvs --rows --headings full -o $OPTS > out
grep "pv_ba_start 0" out
grep "seg_start 0" out
grep "seg_start_pe 0" out


aux lvmconf 'report/headings=2'

pvs -o $OPTS > out
grep "pv_ba_start seg_start seg_start_pe" out

pvs --headings 1 -o $OPTS > out
grep "BA Start Start Start" out

pvs --headings abbrev -o $OPTS > out
grep "BA Start Start Start" out

pvs --headings 2 -o $OPTS > out
grep "pv_ba_start seg_start seg_start_pe" out

pvs --headings full -o $OPTS > out
grep "pv_ba_start seg_start seg_start_pe" out

pvs --rows -o $OPTS > out
grep "pv_ba_start 0" out
grep "seg_start 0" out
grep "seg_start_pe 0" out

pvs --rows --headings 1 -o $OPTS > out
grep "BA Start" out
grep "Start 0" out;

pvs --rows --headings abbrev -o $OPTS > out
grep "BA Start" out
grep "Start 0" out;

pvs --rows --headings 2 -o $OPTS > out
grep "pv_ba_start 0" out
grep "seg_start 0" out
grep "seg_start_pe 0" out

pvs --rows --headings full -o $OPTS > out
grep "pv_ba_start 0" out
grep "seg_start 0" out
grep "seg_start_pe 0" out


# if report/headings=100 (out of bound value), then it is as if "1" was used
aux lvmconf 'report/headings=100'

pvs -o $OPTS > out
grep "BA Start Start Start" out


# if using --nameprefixes, the report/headings=2 is ignored
pvs --headings 2 --nameprefixes -o $OPTS > out
grep "LVM2_PV_BA_START='0 ' LVM2_SEG_START='0 ' LVM2_SEG_START_PE='0'" out

# --noheadings and --headings not allowed
not pvs --headings 1 --noheadings
