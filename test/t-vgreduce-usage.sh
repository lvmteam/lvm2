#!/bin/sh
# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description='Test vgreduce command options for validity'
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
   lv1=$(this_test_)-test-lv1-$$          &&
   lv2=$(this_test_)-test-lv2-$$          &&
   lv3=$(this_test_)-test-lv3-$$'

#TODO --removemissing (+ -- mirrorsonly)

for mdatype in 1 2
do
test_expect_success \
  "(lvm$mdatype) setup PVs" '
   pvcreate -M$mdatype $d1 $d2 
'

test_expect_success \
  "(lvm$mdatype) vgreduce removes only the specified pv from vg (bz427382)" '
   vgcreate -M$mdatype $vg1 $d1 $d2 &&
   vgreduce $vg1 $d1 &&
   check_pv_field_ $d2 vg_name $vg1 &&
   vgremove -f $vg1
'

test_expect_success \
  "(lvm$mdatype) vgreduce rejects removing the last pv (--all)" '
   vgcreate -M$mdatype $vg1 $d1 $d2 &&
   { vgreduce --all $vg1;
     status=$?; echo status=$status; test $status != 0 &&
     vgremove -f $vg1
   }
'

test_expect_success \
  "(lvm$mdatype) vgreduce rejects removing the last pv" '
   vgcreate -M$mdatype $vg1 $d1 $d2 &&
   { vgreduce $vg1 $d1 $d2;
     status=$?; echo status=$status; test $status = 5 &&
     vgremove -f $vg1
   }
'

test_expect_success \
  "(lvm$mdatype) remove PVs " '
  pvremove -ff $d1 $d2 
'
done

for mdatype in 2
do
test_expect_success \
  "(lvm$mdatype) setup PVs (--metadatacopies 0)" '
  pvcreate -M$mdatype $d1 $d2 &&
  pvcreate --metadatacopies 0 -M$mdatype $d3 $d4
'

test_expect_success \
  "(lvm$mdatype) vgreduce rejects removing pv with the last mda copy" '
   vgcreate -M$mdatype $vg1 $d1 $d3 &&
   { vgreduce $vg1 $d1;
     status=$?; echo status=$status; test $status != 0 &&
     vgremove -f $vg1
   }
'
done


test_done
# Local Variables:
# indent-tabs-mode: nil
# End:
