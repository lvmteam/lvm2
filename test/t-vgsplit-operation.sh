#!/bin/sh
# Copyright (C) 2007 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Test vgsplit operation, including different LV types'
privileges_required_=1

. ./test-lib.sh

cleanup_()
{
  test -n "$d1" && losetup -d "$d1"
  test -n "$d2" && losetup -d "$d2"
  test -n "$d3" && losetup -d "$d3"
  test -n "$d4" && losetup -d "$d4"
  rm -f "$f1" "$f2" "$f3" "$f4"
}

validate_vg_pvlv_counts_()
{
	local local_vg=$1
	local num_pvs=$2
	local num_lvs=$3
	local num_snaps=$4

	check_vg_field_ $local_vg pv_count $num_pvs &&
	check_vg_field_ $local_vg lv_count $num_lvs &&
	check_vg_field_ $local_vg snap_count $num_snaps
}

# FIXME: paramaterize lvm1 vs lvm2 metadata; most of these tests should run
# fine with lvm1 metadata as well; for now, just add disks 5 and 6 as lvm1
# metadata
test_expect_success \
  'set up temp files, loopback devices, PVs, vgnames' \
  'f1=$(pwd)/1 && d1=$(loop_setup_ "$f1") &&
   f2=$(pwd)/2 && d2=$(loop_setup_ "$f2") &&
   f3=$(pwd)/3 && d3=$(loop_setup_ "$f3") &&
   f4=$(pwd)/4 && d4=$(loop_setup_ "$f4") &&
   vg1=$(this_test_)-test-vg1-$$          &&
   vg2=$(this_test_)-test-vg2-$$          &&
   lv1=$(this_test_)-test-lv1-$$          &&
   lv2=$(this_test_)-test-lv2-$$          &&
   lv3=$(this_test_)-test-lv3-$$          &&
   pvcreate $d1 $d2 $d3 $d4'

test_expect_success \
  'vgsplit correctly splits single linear LV into existing VG' \
  'vgcreate $vg1 $d1 $d2 &&
   vgcreate $vg2 $d3 $d4 &&
   lvcreate -l 4 -n $lv1 $vg1 $d1 &&
   vgchange -an $vg1 &&
   vgsplit $vg1 $vg2 $d1 &&
   validate_vg_pvlv_counts_ $vg1 1 0 0 &&
   validate_vg_pvlv_counts_ $vg2 3 1 0 &&
   lvremove -f $vg2/$lv1 &&
   vgremove -f $vg2 &&
   vgremove -f $vg1'

test_expect_success \
  'vgsplit correctly splits single striped LV into existing VG' \
  'vgcreate $vg1 $d1 $d2 &&
   vgcreate $vg2 $d3 $d4 &&
   lvcreate -l 4 -i 2 -n $lv1 $vg1 $d1 $d2 &&
   vgchange -an $vg1 &&
   vgsplit $vg1 $vg2 $d1 $d2 &&
   validate_vg_pvlv_counts_ $vg2 4 1 0 &&
   lvremove -f $vg2/$lv1 &&
   vgremove -f $vg2'

test_expect_success \
  'vgsplit correctly splits origin and snapshot LV into existing VG' \
  'vgcreate $vg1 $d1 $d2 &&
   vgcreate $vg2 $d3 $d4 &&
   lvcreate -l 64 -i 2 -n $lv1 $vg1 $d1 $d2 &&
   lvcreate -l 4 -i 2 -s -n $lv2 $vg1/$lv1 &&
   vgchange -an $vg1 &&
   vgsplit $vg1 $vg2 $d1 $d2 &&
   validate_vg_pvlv_counts_ $vg2 4 1 1 &&
   lvremove -f $vg2/$lv2 &&
   lvremove -f $vg2/$lv1 &&
   vgremove -f $vg2'

test_expect_success \
  'vgsplit correctly splits mirror LV into existing VG' \
  'vgcreate $vg1 $d1 $d2 $d3 &&
   vgcreate $vg2 $d4 &&
   lvcreate -l 64 -m1 -n $lv1 $vg1 $d1 $d2 $d3 &&
   vgchange -an $vg1 &&
   vgsplit $vg1 $vg2 $d1 $d2 $d3 &&
   validate_vg_pvlv_counts_ $vg2 4 4 0 &&
   lvremove -f $vg2/$lv1 &&
   vgremove -f $vg2'

test_done
# Local Variables:
# indent-tabs-mode: nil
# End:
