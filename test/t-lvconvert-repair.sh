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

. ./test-utils.sh

prepare_vg 5

# fail multiple devices

lvcreate -m 3 -L 1 -n 3way $vg
disable_dev $dev2 $dev4
echo n | lvconvert -i 1 --repair $vg/3way
lvs -a -o +devices > lvlist
not grep unknown lvlist
vgreduce --removemissing $vg
enable_dev $dev2 $dev4
lvchange -a n $vg/3way

vgremove -ff $vg
vgcreate -c n $vg $dev1 $dev2 $dev3 $dev4

lvcreate -m 2 -L 1 -n 4way $vg
disable_dev $dev1 $dev2
echo n | lvconvert -i 1 --repair $vg/4way
lvs -a -o +devices > lvlist
not grep unknown lvlist
vgreduce --removemissing $vg
enable_dev $dev1 $dev2
lvchange -a n $vg/4way

# fail single devices

vgremove -ff $vg
vgcreate -c n $vg $dev1 $dev2 $dev3

lvcreate -m 1 -L 1 -n mirror $vg

lvchange -a n $vg/mirror
vgextend $vg $dev4
disable_dev $dev1
lvchange --partial -a y $vg/mirror

not vgreduce -v --removemissing $vg
lvconvert -y -i 1 --repair $vg/mirror
vgreduce --removemissing $vg

enable_dev $dev1
vgextend $vg $dev1
disable_dev $dev2
lvconvert -y -i 1 --repair $vg/mirror
vgreduce --removemissing $vg

enable_dev $dev2
vgextend $vg $dev2
disable_dev $dev3
lvconvert -y -i 1 --repair $vg/mirror
vgreduce --removemissing $vg

enable_dev $dev3
vgextend $vg $dev3
lvcreate -m 2 -l 1 -n mirror2 $vg $dev1 $dev2 $dev3 $dev4
vgchange -a n $vg
pvremove -ff -y $dev4
echo 'y' | not lvconvert -y -i 1 --repair $vg/mirror2
vgs
