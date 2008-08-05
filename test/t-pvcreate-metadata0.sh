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
# Testcase for bugzilla #450651
# also checks that vgremove properly removes all lv devices in the right order
#
test_description='Test pvcreate without metadata on all pvs'
privileges_required_=1

. ./test-lib.sh

cleanup_()
{
  test -n "$d1" && losetup -d "$d1"
  test -n "$d2" && losetup -d "$d2"
  rm -f "$f1" "$f2"
}

test_expect_success "set up temp files, loopback devices" \
  'f1=$(pwd)/1 && d1=$(loop_setup_ "$f1") &&
   f2=$(pwd)/2 && d2=$(loop_setup_ "$f2") &&
   vg=$(this_test_)-test-vg-$$            &&
   lv=$(this_test_)-test-lv-$$            &&
   lv_snap=$(this_test_)-test-lv-snap-$$  &&
   pvcreate "$d1"                         &&
   pvcreate --metadatacopies 0 "$d2"'

test_expect_success "check lv snapshot" \
  'vgcreate -c n "$vg" "$d1" "$d2"                  &&
   lvcreate -n "$lv" -l 60%FREE "$vg"               &&
   lvcreate -s -n "$lv_snap" -l 10%FREE "$vg"/"$lv" &&
   pvdisplay                                        &&
   lvdisplay                                        &&
   vgremove -f "$vg"'

test_done

# Local Variables:
# indent-tabs-mode: nil
# End:
