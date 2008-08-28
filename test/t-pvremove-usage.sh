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

test_description='Test pvremove'
privileges_required_=1

. ./test-lib.sh

cleanup_()
{
  test -n "$d1" && losetup -d "$d1"
  test -n "$d2" && losetup -d "$d2"
  test -n "$d3" && losetup -d "$d3"
  rm -f "$f1" "$f2"
}

test_expect_success "set up temp files, loopback devices" \
  'f1=$(pwd)/1 && d1=$(loop_setup_ "$f1") &&
   f2=$(pwd)/2 && d2=$(loop_setup_ "$f2") &&
   f3=$(pwd)/3 && d3=$(loop_setup_ "$f3") &&
   vg=$(this_test_)-test-vg-$$            &&
   pvcreate "$d1"                         &&
   pvcreate --metadatacopies 0 "$d2"      &&
   pvcreate --metadatacopies 2 "$d3"
'

test_expect_success "check pvremove fails when bogus pv given" '
   pvremove "$d2" bogus;
   status=$?; echo $status; test $status != 0 
'

#failing, but still removing everything what can be removed
#is somewhat odd as default, what do we have -f for?
test_expect_failure "but still removes the valid pv that was given too :-/" '
   pvs | grep "$d2"; status=$?;
   pvcreate  --metadatacopies 0 "$d2";
   echo $status; test $status = 0
'

test_expect_success "check pvremove refuses to remove pv in a vg" '
   vgcreate -c n "$vg" "$d1" "$d2" &&
   { pvremove "$d2" "$d3";
     status=$?; echo $status; test $status != 0
   }
'

for mdacp in 0 1 2; do
test_expect_success \
  "check pvremove truly wipes the label (pvscan wont find) (---metadatacopies $mdacp)" '
   pvcreate --metadatacopies $mdacp "$d3" &&
   pvremove "$d3" &&
   { pvscan |grep "$d3"; 
     status=$?; echo $status; test $status != 0 
   }
'

test_expect_success "reset setup" '
   vgremove -ff $vg &&
   pvcreate --metadatacopies $mdacp "$d1" &&
   pvcreate "$d2" &&
   vgcreate $vg $d1 $d2 
'
test_expect_success "pvremove -f fails when pv in a vg (---metadatacopies $mdacp)" '
   pvremove -f $d1;
   status=$?; echo $status; test $status != 0 &&
   pvs $d1
'
test_expect_success \
  "pvremove -ff fails without confirmation when pv in a vg (---metadatacopies $mdacp)" '
   echo n|eval pvremove -ff $d1;
   status=$?; echo $status; test $status != 0
'
test_expect_success \
  "pvremove -ff succeds with confirmation when pv in a vg (---metadatacopies $mdacp)" '
   yes | pvremove -ff $d1 &&
   pvs $d1;
   status=$?; echo $status; test $status != 0
'
test_expect_success "cleanup & setup" '
  vgreduce --removemissing $vg &&
  pvcreate --metadatacopies $mdacp "$d1" &&
  vgextend $vg $d1
'
test_expect_success \
  "pvremove -ff -y is sufficient when pv in a vg (---metadatacopies $mdacp)" '
   echo n | pvremove -ff -y $d1
'
test_expect_success "cleanup & setup" '
  vgreduce --removemissing $vg &&
  pvcreate --metadatacopies $mdacp "$d1" &&
  vgextend $vg $d1
'
done

test_expect_success "cleanup" '
   vgremove -ff "$vg"
'

test_done

# Local Variables:
# indent-tabs-mode: nil
# End:
