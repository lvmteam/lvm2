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

aux prepare_vg 6
aux lvmconf 'allocation/maximise_cling = 0'
aux lvmconf 'allocation/mirror_logs_require_separate_pvs = 1'

# 3-way, disk log
# multiple failures, full replace
lvcreate --mirrorlog disk -m 2 --ig -L 1 -n 3way $vg $dev1 $dev2 $dev3 $dev4:0-1
aux disable_dev $dev1 $dev2
echo y | lvconvert --repair $vg/3way 2>&1 | tee 3way.out
lvs -a -o +devices | not grep unknown
not grep "WARNING: Failed" 3way.out
vgreduce --removemissing $vg
check mirror $vg 3way
aux enable_dev $dev1 $dev2

vgremove -ff $vg; vgcreate -c n $vg $dev1 $dev2 $dev3 $dev4 $dev5 $dev6

# 2-way, mirrored log
# Double log failure, full replace
lvcreate --mirrorlog mirrored -m 1 --ig -L 1 -n 2way $vg \
    $dev1 $dev2 $dev3:0 $dev4:0
aux disable_dev $dev3 $dev4
echo y | lvconvert --repair $vg/2way 2>&1 | tee 2way.out
lvs -a -o +devices | not grep unknown
not grep "WARNING: Failed" 2way.out
vgreduce --removemissing $vg
check mirror $vg 2way
aux enable_dev $dev3 $dev4

vgremove -ff $vg; vgcreate -c n $vg $dev1 $dev2 $dev3 $dev4 $dev5 $dev6

# 3-way, mirrored log
# Single log failure, replace
lvcreate --mirrorlog mirrored -m 2 --ig -L 1 -n 3way $vg \
    $dev1 $dev2 $dev3 $dev4:0 $dev5:0
aux disable_dev $dev4
echo y | lvconvert --repair $vg/3way 2>&1 | tee 3way.out
lvs -a -o +devices | not grep unknown
not grep "WARNING: Failed" 3way.out
vgreduce --removemissing $vg
check mirror $vg 3way
aux enable_dev $dev4

vgremove -ff $vg; vgcreate -c n $vg $dev1 $dev2 $dev3 $dev4 $dev5

# 3-way, disk log
# multiple failures, partial replace
lvcreate --mirrorlog disk -m 2 --ig -L 1 -n 3way $vg $dev1 $dev2 $dev3 $dev4
aux disable_dev $dev1 $dev2
echo y | lvconvert --repair $vg/3way 2>&1 | tee 3way.out
grep "WARNING: Failed" 3way.out
lvs -a -o +devices | not grep unknown
vgreduce --removemissing $vg
check mirror $vg 3way
aux enable_dev $dev1 $dev2
lvchange -a n $vg/3way

vgremove -ff $vg; vgcreate -c n $vg $dev1 $dev2 $dev3

lvcreate --mirrorlog disk -m 1 --ig -L 1 -n 2way $vg $dev1 $dev2 $dev3
aux disable_dev $dev1
echo y | lvconvert --repair $vg/2way 2>&1 | tee 2way.out
grep "WARNING: Failed" 2way.out
lvs -a -o +devices | not grep unknown
vgreduce --removemissing $vg
check mirror $vg 2way
aux enable_dev $dev1 $dev2
lvchange -a n $vg/2way

vgremove -ff $vg; vgcreate -c n $vg $dev1 $dev2 $dev3 $dev4

# Test repair of inactive mirror with log failure
#  Replacement should fail, but covert should succeed (switch to corelog)
lvcreate -m 2 --ig -l 2 -n mirror2 $vg $dev1 $dev2 $dev3 $dev4:0
vgchange -a n $vg
pvremove -ff -y $dev4
echo 'y' | lvconvert -y --repair $vg/mirror2
check mirror $vg mirror2
vgs

