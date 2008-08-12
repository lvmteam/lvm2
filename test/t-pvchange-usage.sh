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

test_description='Test pvchange option values'
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
  'set up temp files, loopback devices, PVs, vgname' \
  'f1=$(pwd)/1 && d1=$(loop_setup_ "$f1") &&
   f2=$(pwd)/2 && d2=$(loop_setup_ "$f2") &&
   f3=$(pwd)/3 && d3=$(loop_setup_ "$f3") &&
   f4=$(pwd)/4 && d4=$(loop_setup_ "$f4") &&
   vg1=$(this_test_)-test-vg1-$$ &&
   lv=$(this_test_)-test-lv-$$'

for mda in 0 1 2 
do
test_expect_success \
  "setup pv with metadatacopies = $mda" '
   pvcreate $d4 &&
   pvcreate --metadatacopies $mda $d1 &&
   vgcreate $vg1 $d1 $d4 
'

test_expect_success \
  "pvchange adds/dels tag to pvs with metadatacopies = $mda " '
   pvchange $d1 --addtag test$mda &&
   check_pv_field_ $d1 pv_tags test$mda &&
   pvchange $d1 --deltag test$mda &&
   check_pv_field_ $d1 pv_tags " "
'

test_expect_success \
  "vgchange disable/enable allocation for pvs with metadatacopies = $mda" '
   pvchange $d1 -x n &&
   check_pv_field_ $d1 pv_attr  --  &&
   pvchange $d1 -x y &&
   check_pv_field_ $d1 pv_attr  a- 
'

test_expect_success \
  'remove pv' '
   vgremove $vg1 &&
   pvremove $d1 $d4
'
done

test_expect_success \
  "pvchange uuid" "
   pvcreate --metadatacopies 0 $d1 &&
   pvcreate --metadatacopies 2 $d2 &&
   vgcreate $vg1 $d1 $d2 &&
   pvchange -u $d1 &&
   pvchange -u $d2 &&
   vg_validate_pvlv_counts_ $vg1 2 0 0
"
test_expect_success \
  "pvchange rejects uuid change under an active lv" '
   lvcreate -l 16 -i 2 -n $lv --alloc anywhere $vg1 &&
   vg_validate_pvlv_counts_ $vg1 2 1 0 &&
   pvchange -u $d1;
   status=$?; echo status=$status; test $status = 5 &&
   lvchange -an "$vg1"/"$lv" &&
   pvchange -u $d1
'

test_expect_success \
  "cleanup" '
   lvremove -f "$vg1"/"$lv" &&
   vgremove $vg1
'

test_done
# Local Variables:
# indent-tabs-mode: nil
# End:
