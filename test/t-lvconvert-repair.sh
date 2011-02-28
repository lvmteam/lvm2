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

aux lvmconf 'allocation/maximise_cling = 0'
aux lvmconf 'allocation/mirror_logs_require_separate_pvs = 1'

# fail multiple devices

# 4-way, disk log => 2-way, disk log
aux prepare_vg 5
lvcreate -m 3 --ig -L 1 -n 4way $vg $dev1 $dev2 $dev3 $dev4 $dev5:0
aux disable_dev $dev2 $dev4
echo n | lvconvert --repair $vg/4way 2>&1 | tee 4way.out
lvs -a -o +devices | not grep unknown
vgreduce --removemissing $vg
aux enable_dev $dev2 $dev4
check mirror $vg 4way $dev5

# 3-way, disk log => linear
aux prepare_vg 5
lvcreate -m 2 --ig -L 1 -n 3way $vg
aux disable_dev $dev1 $dev2
echo n | lvconvert --repair $vg/3way
check linear $vg 3way
lvs -a -o +devices | not grep unknown
lvs -a -o +devices | not grep mlog
dmsetup ls | grep $PREFIX | not grep mlog
vgreduce --removemissing $vg
aux enable_dev $dev1 $dev2
check linear $vg 3way

# fail just log and get it removed

# 3-way, disk log => 3-way, core log
aux prepare_vg 5
lvcreate -m 2 --ig -L 1 -n 3way $vg $dev1 $dev2 $dev3 $dev4:0
aux disable_dev $dev4
echo n | lvconvert --repair $vg/3way
check mirror $vg 3way core
lvs -a -o +devices | not grep unknown
lvs -a -o +devices | not grep mlog
dmsetup ls | grep $PREFIX | not grep mlog
vgreduce --removemissing $vg
aux enable_dev $dev4

# 3-way, mirrored log => 3-way, core log
aux prepare_vg 5
lvcreate -m 2 --mirrorlog mirrored --ig -L 1 -n 3way $vg \
    $dev1 $dev2 $dev3 $dev4:0 $dev5:0
aux disable_dev $dev4 $dev5
echo n | lvconvert --repair $vg/3way
check mirror $vg 3way core
lvs -a -o +devices | not grep unknown
lvs -a -o +devices | not grep mlog
dmsetup ls | grep $PREFIX | not grep mlog
vgreduce --removemissing $vg
aux enable_dev $dev4 $dev5

# 2-way, disk log => 2-way, core log
aux prepare_vg 5
lvcreate -m 1 --ig -L 1 -n 2way $vg $dev1 $dev2 $dev3:0
aux disable_dev $dev3
echo n | lvconvert --repair $vg/2way
check mirror $vg 2way core
lvs -a -o +devices | not grep unknown
lvs -a -o +devices | not grep mlog
vgreduce --removemissing $vg
aux enable_dev $dev3

# fail single devices

aux prepare_vg 5
vgreduce $vg $dev4

lvcreate -m 1 --ig -L 1 -n mirror $vg
lvchange -a n $vg/mirror
vgextend $vg $dev4
aux disable_dev $dev1
lvchange --partial -a y $vg/mirror

not vgreduce -v --removemissing $vg
lvconvert -y --repair $vg/mirror
vgreduce --removemissing $vg

aux enable_dev $dev1
vgextend $vg $dev1
aux disable_dev $dev2
lvconvert -y --repair $vg/mirror
vgreduce --removemissing $vg

aux enable_dev $dev2
vgextend $vg $dev2
aux disable_dev $dev3
lvconvert -y --repair $vg/mirror
vgreduce --removemissing $vg
aux enable_dev $dev3
vgextend $vg $dev3
lvremove -ff $vg
