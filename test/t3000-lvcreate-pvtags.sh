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

test_description='Ensure that pvmove diagnoses PE-range values 2^32 and larger.'
privileges_required_=1

. ./test-lib.sh

cleanup_()
{
  test -n "$vg" && {
    lvremove -ff $vg
    vgremove $vg
  } > /dev/null
  test -n "$pvs" && {
    pvremove $pvs > /dev/null
    for d in $pvs; do
      dmsetup remove $(basename $d)
    done
  }
  losetup -d $lodev
  rm -f $lofile
}

nr_pvs=3
pvsize=$((200 * 1024 * 2))

test_expect_success \
  'set up temp file and loopback device' \
  'lofile="$(pwd)/lofile" && lodev=$(loop_setup_ "$lofile")'

offset=0
pvs=
for n in $(seq 1 $nr_pvs); do
  test_expect_success \
      "create pv$n" \
      'echo "0 $pvsize linear $lodev $offset" > in &&
       dmsetup create pv$n < in'
  offset=$(($offset + $pvsize))
done

for n in $(seq 1 $nr_pvs); do
  pvs="$pvs /dev/mapper/pv$n"
done

test_expect_success \
  "Run this: pvcreate $pvs" \
  'pvcreate $pvs'

vg=lvcreate-pvtags-vg-$$
test_expect_success "Run this: vgcreate $vg $pvs" \
  'vgcreate $vg $pvs'
test_expect_success "Run this: pvchange --addtag fast $pvs" \
  'pvchange --addtag fast $pvs'

test_expect_success t 'lvcreate -l3 -i3 $vg @fast'
test_expect_failure u 'lvcreate -l4 -i4 $vg @fast'
test_expect_failure v 'lvcreate -l2 -i2 $vg /dev/mapper/pv1'

test_expect_success 'lvcreate mirror'           \
  'lvcreate -l1 -m1 $vg @fast'
test_expect_success 'lvcreate mirror corelog'   \
  'lvcreate -l1 -m2 --corelog $vg @fast'
test_expect_failure 'lvcreate mirror'           \
  'lvcreate -l1 -m2 $vg @fast'
test_expect_failure 'lvcreate mirror (corelog)' \
  'lvcreate -l1 -m3 --corelog $vg @fast'
test_expect_failure 'lvcreate mirror'           \
  'lvcreate -l1 -m1 --corelog $vg /dev/mapper/pv1'

test_done
