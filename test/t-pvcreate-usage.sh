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

test_description='Test pvcreate option values'
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

test_expect_success \
  'pvcreate rejects negative setphysicalvolumesize' \
  'pvcreate --setphysicalvolumesize -1024 $d1;
   status=$?; echo status=$status; test $status != 0'

test_expect_success \
  'pvcreate rejects negative metadatasize' \
  'pvcreate --metadatasize -1024 $d1;
   status=$?; echo status=$status; test $status != 0'

# x. metadatasize 0, defaults to 255
# FIXME: unable to check default value, not in reporting cmds
# should default to 255 according to code
#   check_pv_field_ pv_mda_size 255 &&
test_expect_success \
  'pvcreate accepts metadatasize 0' \
  'pvcreate --metadatasize 0 $d1 &&
   pvremove $d1'

# x. metadatasize too large
# For some reason we allow this, even though there's no room for data?
#test_expect_success \
#  'pvcreate rejects metadatasize too large' \
#  'pvcreate --metadatasize 100000000000000 $d1;
#   status=$?; echo status=$status; test $status != 0'

test_expect_success \
  'pvcreate rejects metadatacopies < 0' \
  'pvcreate --metadatacopies -1 $d1;
   status=$?; echo status=$status; test $status != 0'

test_expect_success \
  'pvcreate accepts metadatacopies = 0, 1, 2' \
  'pvcreate --metadatacopies 0 $d1 &&
   pvcreate --metadatacopies 1 $d2 &&
   pvcreate --metadatacopies 2 $d3 &&
   check_pv_field_ $d1 pv_mda_count 0 &&
   check_pv_field_ $d2 pv_mda_count 1 &&
   check_pv_field_ $d3 pv_mda_count 2 &&
   pvremove $d1 &&
   pvremove $d2 &&
   pvremove $d3'

test_expect_success \
  'pvcreate rejects metadatacopies > 2' \
  'pvcreate --metadatacopies 3 $d1;
   status=$?; echo status=$status; test $status != 0'

test_expect_success \
  'pvcreate rejects invalid device' \
  'pvcreate $d1bogus;
   status=$?; echo status=$status; test $status != 0'

test_expect_success \
  'pvcreate rejects labelsector < 0' \
  'pvcreate --labelsector -1 $d1;
   status=$?; echo status=$status; test $status != 0'

test_expect_success \
  'pvcreate rejects labelsector > 1000000000000' \
  'pvcreate --labelsector 1000000000000 $d1;
   status=$?; echo status=$status; test $status != 0'

# other possibilites based on code inspection (not sure how hard)
# x. device too small (min of 512 * 1024 KB)
# x. device filtered out
# x. unable to open /dev/urandom RDONLY
# x. device too large (pe_count > UINT32_MAX)
# x. device read-only
# x. unable to open device readonly
# x. BLKGETSIZE64 fails
# x. set size to value inconsistent with device / PE size

test_done
# Local Variables:
# indent-tabs-mode: nil
# End:
