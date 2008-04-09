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

vg_validate_pvlv_counts_()
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

#
# vgsplit can be done into a new or existing VG
#
for i in new existing
do
#
# We can have PVs or LVs on the cmdline
#
for j in PV LV
do
test_expect_success \
  "vgsplit correctly splits single linear LV into $i VG ($j args)" \
  'vgcreate $vg1 $d1 $d2 &&
   if [ $i == existing ]; then
      vgcreate $vg2 $d3 $d4
   fi &&
   lvcreate -l 4 -n $lv1 $vg1 $d1 &&
   vgchange -an $vg1 &&
   if [ $j == PV ]; then
     vgsplit $vg1 $vg2 $d1
   else
     vgsplit -n $lv1 $vg1 $vg2
   fi &&
   vg_validate_pvlv_counts_ $vg1 1 0 0 &&
   if [ $i == existing ]; then
      vg_validate_pvlv_counts_ $vg2 3 1 0
   else
      vg_validate_pvlv_counts_ $vg2 1 1 0
   fi &&
   lvremove -f $vg2/$lv1 &&
   vgremove -f $vg2 &&
   vgremove -f $vg1'

test_expect_success \
  "vgsplit correctly splits single striped LV into $i VG ($j args)" \
  'vgcreate $vg1 $d1 $d2 &&
   if [ $i == existing ]; then
      vgcreate $vg2 $d3 $d4
   fi &&
   lvcreate -l 4 -i 2 -n $lv1 $vg1 $d1 $d2 &&
   vgchange -an $vg1 &&
   if [ $j == PV ]; then
     vgsplit $vg1 $vg2 $d1 $d2
   else
     vgsplit -n $lv1 $vg1 $vg2
   fi &&
   if [ $i == existing ]; then
     vg_validate_pvlv_counts_ $vg2 4 1 0
   else
     vg_validate_pvlv_counts_ $vg2 2 1 0
   fi &&
   lvremove -f $vg2/$lv1 &&
   vgremove -f $vg2'

test_expect_success \
  "vgsplit correctly splits mirror LV into $i VG ($j args)" \
  'vgcreate $vg1 $d1 $d2 $d3 &&
   if [ $i == existing ]; then
     vgcreate $vg2 $d4
   fi &&
   lvcreate -l 64 -m1 -n $lv1 $vg1 $d1 $d2 $d3 &&
   vgchange -an $vg1 &&
   if [ $j == PV ]; then
     vgsplit $vg1 $vg2 $d1 $d2 $d3
   else
     vgsplit -n $lv1 $vg1 $vg2
   fi &&
   if [ $i == existing ]; then
     vg_validate_pvlv_counts_ $vg2 4 4 0
   else
     vg_validate_pvlv_counts_ $vg2 3 4 0
   fi &&
   lvremove -f $vg2/$lv1 &&
   vgremove -f $vg2'

test_expect_success \
  "vgsplit correctly splits origin and snapshot LV into $i VG ($j args)" \
  'vgcreate $vg1 $d1 $d2 &&
   if [ $i == existing ]; then
     vgcreate $vg2 $d3 $d4
   fi &&
   lvcreate -l 64 -i 2 -n $lv1 $vg1 $d1 $d2 &&
   lvcreate -l 4 -i 2 -s -n $lv2 $vg1/$lv1 &&
   vgchange -an $vg1 &&
   if [ $j == PV ]; then
     vgsplit $vg1 $vg2 $d1 $d2
   else
     vgsplit -n $lv1 $vg1 $vg2
   fi &&
   if [ $i == existing ]; then
     vg_validate_pvlv_counts_ $vg2 4 1 1
   else
     vg_validate_pvlv_counts_ $vg2 2 1 1
   fi &&
   lvremove -f $vg2/$lv2 &&
   lvremove -f $vg2/$lv1 &&
   vgremove -f $vg2'

test_expect_success \
  "vgsplit correctly splits linear LV but not snap+origin LV into $i VG ($j args)" \
  'vgcreate $vg1 $d1 $d2 &&
   if [ $i == existing ]; then
     vgcreate $vg2 $d3
   fi &&
   lvcreate -l 64 -i 2 -n $lv1 $vg1 &&
   lvcreate -l 4 -i 2 -s -n $lv2 $vg1/$lv1 &&
   vgextend $vg1 $d4 &&
   lvcreate -l 64 -n $lv3 $vg1 $d4 &&
   vgchange -an $vg1 &&
   if [ $j == PV ]; then
     vgsplit $vg1 $vg2 $d4
   else
     vgsplit -n $lv3 $vg1 $vg2
   fi &&
   if [ $i == existing ]; then
     vg_validate_pvlv_counts_ $vg2 2 1 0
     vg_validate_pvlv_counts_ $vg1 2 1 1
   else
     vg_validate_pvlv_counts_ $vg2 1 1 0
     vg_validate_pvlv_counts_ $vg1 2 1 1
   fi &&
   lvremove -f $vg1/$lv2 &&
   lvremove -f $vg1/$lv1 &&
   lvremove -f $vg2/$lv3 &&
   vgremove -f $vg1 &&
   vgremove -f $vg2'

done
done

#
# Test more complex setups where the code has to find associated PVs and
# LVs to split the VG correctly
# 
test_expect_success \
  "vgsplit fails splitting 3 striped LVs into VG when only 1 LV specified" \
  'vgcreate $vg1 $d1 $d2 $d3 $d4 &&
   lvcreate -l 4 -n $lv1 -i 2 $vg1 $d1 $d2 &&
   lvcreate -l 4 -n $lv2 -i 2 $vg1 $d2 $d3 &&
   lvcreate -l 4 -n $lv3 -i 2 $vg1 $d3 $d4 &&
   vgchange -an $vg1 &&
   vgsplit -n $lv1 $vg1 $vg2;
   status=$?; echo status=$?; test $status = 5 &&
   vgremove -ff $vg1'

test_expect_success \
  "vgsplit fails splitting one LV with 2 snapshots, only origin LV specified" \
  'vgcreate $vg1 $d1 $d2 $d3 $d4 &&
   lvcreate -l 16 -n $lv1 $vg1 $d1 $d2 &&
   lvcreate -l 4 -n $lv2 -s $vg1/$lv1 &&
   lvcreate -l 4 -n $lv3 -s $vg1/$lv1 &&
   vg_validate_pvlv_counts_ $vg1 4 1 2 &&
   vgchange -an $vg1 &&
   vgsplit -n $lv1 $vg1 $vg2;
   status=$?; echo status=$?; test $status = 5 &&
   lvremove -f $vg1/$lv2 &&
   lvremove -f $vg1/$lv3 &&
   lvremove -f $vg1/$lv1 &&
   vgremove -ff $vg1'

test_expect_success \
  "vgsplit fails splitting one LV with 2 snapshots, only snapshot LV specified" \
  'vgcreate $vg1 $d1 $d2 $d3 $d4 &&
   lvcreate -l 16 -n $lv1 $vg1 $d1 $d2 &&
   lvcreate -l 4 -n $lv2 -s $vg1/$lv1 &&
   lvcreate -l 4 -n $lv3 -s $vg1/$lv1 &&
   vg_validate_pvlv_counts_ $vg1 4 1 2 &&
   vgchange -an $vg1 &&
   vgsplit -n $lv2 $vg1 $vg2;
   status=$?; echo status=$?; test $status = 5 &&
   lvremove -f $vg1/$lv2 &&
   lvremove -f $vg1/$lv3 &&
   lvremove -f $vg1/$lv1 &&
   vgremove -ff $vg1'

test_expect_success \
  "vgsplit fails splitting one mirror LV, only one PV specified" \
  'vgcreate $vg1 $d1 $d2 $d3 $d4 &&
   lvcreate -l 16 -n $lv1 -m1 $vg1 $d1 $d2 $d3 &&
   vg_validate_pvlv_counts_ $vg1 4 4 0 &&
   vgchange -an $vg1 &&
   vgsplit $vg1 $vg2 $d2 &&
   status=$?; echo status=$?; test $status = 5 &&
   vgremove -ff $vg1'

test_expect_success \
  "vgsplit fails splitting 1 mirror + 1 striped LV, only striped LV specified" \
  'vgcreate $vg1 $d1 $d2 $d3 $d4 &&
   lvcreate -l 16 -n $lv1 -m1 $vg1 $d1 $d2 $d3 &&
   lvcreate -l 16 -n $lv2 -i 2 $vg1 $d3 $d4 &&
   vg_validate_pvlv_counts_ $vg1 4 5 0 &&
   vgchange -an $vg1 &&
   vgsplit -n $lv2 $vg1 $vg2 2>err;
   status=$?; echo status=$?; test $status = 5 &&
   vgremove -ff $vg1'


test_done

# Local Variables:
# indent-tabs-mode: nil
# End:
