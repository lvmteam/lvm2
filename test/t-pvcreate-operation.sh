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

test_description='Test pvcreate logic operation'
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
   vg1=$(this_test_)-test-vg1-$$'

for mdatype in 1 2
do
test_expect_success \
  "pvcreate (lvm$mdatype) fails when PV belongs to VG" \
  'pvcreate -M$mdatype $d1 &&
   vgcreate -M$mdatype $vg1 $d1 &&
   pvcreate -M$mdatype $d1;
   status=$?; echo status=$status; test $status != 0 &&
   vgremove -f $vg1 &&
   pvremove -f $d1'

done

test_expect_success \
  'pvcreate (lvm2) fails without -ff when PV with metadatacopies=0 belongs to VG' \
  'pvcreate --metadatacopies 0 $d1 &&
   pvcreate --metadatacopies 1 $d2 &&
   vgcreate $vg1 $d1 $d2 &&
   pvcreate $d1;
   status=$?; echo status=$status; test $status != 0 &&
   vgremove -f $vg1 &&
   pvremove -f $d2 &&
   pvremove -f $d1'

test_expect_success \
  'pvcreate (lvm2) succeeds with -ff when PV with metadatacopies=0 belongs to VG' \
  'pvcreate --metadatacopies 0 $d1 &&
   pvcreate --metadatacopies 1 $d2 &&
   vgcreate $vg1 $d1 $d2 &&
   pvcreate -ff -y $d1 &&
   vgreduce --removemissing $vg1 &&
   vgremove -ff $vg1 &&
   pvremove -f $d2 &&
   pvremove -f $d1'

for i in 0 1 2 3 
do
 test_expect_success \
  "pvcreate (lvm2) succeeds writing LVM label at sector $i" \
  'pvcreate --labelsector $i $d1 &&
  dd if=$d1 bs=512 skip=$i count=1 status=noxfer 2>&1 | strings | grep -q LABELONE;
  test $? == 0 &&
  pvremove -f $d1'
done

test_expect_failure \
  "pvcreate (lvm2) fails writing LVM label at sector 4" \
  'pvcreate --labelsector 4 $d1'

backupfile=mybackupfile-$(this_test_)
uuid1=freddy-fred-fred-fred-fred-fred-freddy
uuid2=freddy-fred-fred-fred-fred-fred-fredie
bogusuuid=fred

test_expect_failure \
  'pvcreate rejects uuid option with less than 32 characters' \
  'pvcreate --uuid $bogusuuid $d1'

test_expect_success \
  'pvcreate rejects uuid already in use' \
  'pvcreate --uuid $uuid1 $d1 &&
   pvcreate --uuid $uuid1 $d2;
   status=$?; echo status=$status; test $status != 0'

test_expect_success \
  'pvcreate rejects non-existent file given with restorefile' \
  'pvcreate --uuid $uuid1 --restorefile $backupfile $d1;
   status=$?; echo status=$status; test $status != 0'

test_expect_success \
  'pvcreate rejects restorefile with uuid not found in file' \
  'pvcreate --uuid $uuid1 $d1 &&
   vgcfgbackup -f $backupfile &&
   pvcreate --uuid $uuid2 --restorefile $backupfile $d2;
   status=$?; echo status=$status; test $status != 0'

test_done
# Local Variables:
# indent-tabs-mode: nil
# End:
