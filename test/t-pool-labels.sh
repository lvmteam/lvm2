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

test_description='Test lvm functionality with GFS pool labels'
privileges_required_=1

. ./test-lib.sh

cleanup_()
{
  test -n "$d1" && losetup -d "$d1"
  rm -f "$f1" "$f2"
}

# create the old GFS pool labeled linear devices
create_pool_label_()
{
  echo -en "\x01\x16\x70\x06\x5f\xcf\xff\xb9\xf8\x24\x8apool1" | dd of=$2 bs=5 seek=1 conv=notrunc
  echo -en "\x04\x01\x03\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x0$1\x68\x01\x16\x70\x00\x00\x00\x00\x00\x06\x5f\xd0" | dd of=$2 bs=273 seek=1 conv=notrunc
}

test_expect_success "set up temp files, loopback devices, pool labels" \
  'f1=$(pwd)/0 && d1=$(loop_setup_ "$f1") &&
   f2=$(pwd)/1 && d2=$(loop_setup_ "$f2") &&
   create_pool_label_ 0 "$d1"             &&
   create_pool_label_ 1 "$d2"'

test_expect_failure "check that pvcreate fails without -ff on the pool device" \
  'pvcreate "$d1"'

test_expect_success "check that vgdisplay and pvcreate -ff works with the pool device" \
  'vgdisplay                         &&
   test -n "$d2" && losetup -d "$d2" &&
   vgdisplay                         &&
   pvcreate -ff -y "$d1"'

test_done

# Local Variables:
# indent-tabs-mode: nil
# End:
