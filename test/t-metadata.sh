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

#
# tests basic functionality of read-ahead and ra regressions
#

test_description='Test --metadatatype 1'
privileges_required_=1

. ./test-lib.sh

cleanup_()
{
   vgremove -f "$vg"
   test -n "$d1" && losetup -d "$d1"
   test -n "$d2" && losetup -d "$d2"
   test -n "$d3" && losetup -d "$d3"
   test -n "$d4" && losetup -d "$d4"
   test -n "$d5" && losetup -d "$d5"
   rm -f "$f1" "$f2" "$f3" "$f4" "$f5"
}

test_expect_success "set up temp files, loopback devices" \
  'f1=$(pwd)/1 && d1=$(loop_setup_ "$f1") &&
   f2=$(pwd)/2 && d2=$(loop_setup_ "$f2") &&
   f3=$(pwd)/3 && d3=$(loop_setup_ "$f3") &&
   f4=$(pwd)/4 && d4=$(loop_setup_ "$f4") &&
   f5=$(pwd)/5 && d5=$(loop_setup_ "$f5") &&
   vg=$(this_test_)-test-vg-$$            &&
   lv=$(this_test_)-test-lv-$$
   pvcreate "$d1"                                    &&
   pvcreate --metadatacopies 0 "$d2"                 &&
   pvcreate --metadatacopies 0 "$d3"                 &&
   pvcreate "$d4"                                    &&
   pvcreate --metadatacopies 0 "$d5"                 &&
   vgcreate -c n "$vg" "$d1" "$d2" "$d3" "$d4" "$d5" &&
   lvcreate -n "$lv" -l 1%FREE -i5 -I256 "$vg"'

test_expect_success "test medatasize 0" \
  'pvchange -x n "$d1"   &&
   pvchange -x y "$d1"   &&
   vgchange -a n "$vg"   &&
   pvchange --uuid "$d1" &&
   pvchange --uuid "$d2" &&
   vgremove -f "$vg"'


test_expect_success "test metadatatype 1" \
  'pvcreate -M1 "$d1"    &&
   pvcreate -M1 "$d2"    &&
   pvcreate -M1 "$d3"    &&
   vgcreate -M1 "$vg" "$d1" "$d2" "$d3" &&
   pvchange --uuid "$d1"'

test_done

# Local Variables:
# indent-tabs-mode: nil
# End:
