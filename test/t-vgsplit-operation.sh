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

test_description='Exercise some vgsplit diagnostics'
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

test_expect_success \
  'set up temp files, loopback devices, PVs, vgnames' \
  'f1=$(pwd)/1 && d1=$(loop_setup_ "$f1") &&
   f2=$(pwd)/2 && d2=$(loop_setup_ "$f2") &&
   f3=$(pwd)/3 && d3=$(loop_setup_ "$f3") &&
   f4=$(pwd)/4 && d4=$(loop_setup_ "$f4") &&
   vg1=$(this_test_)-test-vg1-$$          &&
   vg2=$(this_test_)-test-vg2-$$          &&
   pvcreate $d1 $d2 $d3 $d4'

test_expect_success \
  'vgsplit accepts new vg as destination of split' \
  'vgcreate $vg1 $d1 $d2 &&
   vgsplit $vg1 $vg2 $d1 &&
   vgremove $vg1 &&
   vgremove $vg2'

test_expect_success \
  'vgsplit accepts existing vg as destination of split' \
  'vgcreate $vg1 $d1 $d2 &&
   vgcreate $vg2 $d3 $d4 &&
   vgsplit $vg1 $vg2 $d1 &&
   vgremove $vg1 &&
   vgremove $vg2'

#test_expect_success \
# 'vgcreate accepts 8.00M physicalextentsize for VG' \
#  'vgcreate $vg --physicalextentsize 8.00M $d1 $d2 &&
#   check_vg_field_ $vg vg_extent_size 8.00M &&
#   vgremove $vg'

test_expect_success \
  'vgsplit accepts 8.00M physicalextentsize for new VG' \
  'vgcreate $vg1 $d1 $d2 &&
   vgsplit --physicalextentsize 8.00M $vg1 $vg2 $d1 &&
   check_vg_field_ $vg2 vg_extent_size 8.00M &&
   vgremove $vg1 &&
   vgremove $vg2'

test_expect_success \
  'vgsplit accepts --maxphysicalvolumes 128 on new VG' \
  'vgcreate $vg1 $d1 $d2 &&
   vgsplit --maxphysicalvolumes 128 $vg1 $vg2 $d1 &&
   check_vg_field_ $vg2 max_pv 128 &&
   vgremove $vg1 &&
   vgremove $vg2'

test_expect_success \
  'vgsplit accepts --maxlogicalvolumes 128 on new VG' \
  'vgcreate $vg1 $d1 $d2 &&
   vgsplit --maxlogicalvolumes 128 $vg1 $vg2 $d1 &&
   check_vg_field_ $vg2 max_lv 128 &&
   vgremove $vg1 &&
   vgremove $vg2'

test_done
# Local Variables:
# indent-tabs-mode: nil
# End:
