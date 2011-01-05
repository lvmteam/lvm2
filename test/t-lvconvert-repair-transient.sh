#!/bin/bash
# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux prepare_vg 5

lvcreate -m 3 --ig -L 1 -n 4way $vg
aux disable_dev $dev2 $dev4
mkfs.ext3 $DM_DEV_DIR/$vg/4way &
sleep 1
aux enable_dev $dev2 $dev4
echo n | lvconvert --repair $vg/4way 2>&1 | tee 4way.out
lvs -a -o +devices | not grep unknown
vgreduce --removemissing $vg
check mirror $vg 4way
lvchange -a n $vg/4way
wait
