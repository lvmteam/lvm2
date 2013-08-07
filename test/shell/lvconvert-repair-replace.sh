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

. lib/test

aux prepare_vg 6
aux lvmconf 'allocation/maximise_cling = 0'
aux lvmconf 'allocation/mirror_logs_require_separate_pvs = 1'

# 3-way, disk log
# multiple failures, full replace
lvcreate -aey --mirrorlog disk --type mirror -m 2 --ignoremonitoring --nosync -L 1 -n 3way $vg "$dev1" "$dev2" "$dev3" "$dev4":0-1
aux disable_dev "$dev1" "$dev2"
lvconvert -y --repair $vg/3way 2>&1 | tee 3way.out
lvs -a -o +devices $vg | not grep unknown
not grep "WARNING: Failed" 3way.out
vgreduce --removemissing $vg
check mirror $vg 3way
aux enable_dev "$dev1" "$dev2"
vgremove -ff $vg

# 3-way, disk log
# multiple failures, partial replace
vgcreate $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate -aey --mirrorlog disk --type mirror -m 2 --ignoremonitoring --nosync -L 1 -n 3way $vg "$dev1" "$dev2" "$dev3" "$dev4"
aux disable_dev "$dev1" "$dev2"
lvconvert -y --repair $vg/3way 2>&1 | tee 3way.out
grep "WARNING: Failed" 3way.out
lvs -a -o +devices $vg | not grep unknown
vgreduce --removemissing $vg
check mirror $vg 3way
aux enable_dev "$dev1" "$dev2"
vgremove -ff $vg

vgcreate $vg "$dev1" "$dev2" "$dev3"
lvcreate -aey --mirrorlog disk --type mirror -m 1 --ignoremonitoring --nosync -l 1 -n 2way $vg "$dev1" "$dev2" "$dev3"
aux disable_dev "$dev1"
lvconvert -y --repair $vg/2way 2>&1 | tee 2way.out
grep "WARNING: Failed" 2way.out
lvs -a -o +devices $vg | not grep unknown
vgreduce --removemissing $vg
check mirror $vg 2way
aux enable_dev "$dev1" "$dev2"
vgremove -ff $vg

# Test repair of inactive mirror with log failure
#  Replacement should fail, but convert should succeed (switch to corelog)
vgcreate $vg "$dev1" "$dev2" "$dev3" "$dev4"
lvcreate -aey --type mirror -m 2 --ignoremonitoring --nosync -l 2 -n mirror2 $vg "$dev1" "$dev2" "$dev3" "$dev4":0
vgchange -a n $vg
pvremove -ff -y "$dev4"
lvconvert -y --repair $vg/mirror2
check mirror $vg mirror2
vgs $vg
vgremove -ff $vg

# FIXME  - exclusive activation for mirrors should work here
test -e LOCAL_CLVMD && exit 0

if kernel_at_least 3 0 0; then
	# 2-way, mirrored log
	# Double log failure, full replace
	vgcreate $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"
	lvcreate -aey --mirrorlog mirrored --type mirror -m 1 --ignoremonitoring --nosync -L 1 -n 2way $vg \
	    "$dev1" "$dev2" "$dev3":0 "$dev4":0
	aux disable_dev "$dev3" "$dev4"
	lvconvert -y --repair $vg/2way 2>&1 | tee 2way.out
	lvs -a -o +devices $vg | not grep unknown
	not grep "WARNING: Failed" 2way.out
	vgreduce --removemissing $vg
	check mirror $vg 2way
	aux enable_dev "$dev3" "$dev4"
	vgremove -ff $vg
fi

# 3-way, mirrored log
# Single log failure, replace
vgcreate $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"
lvcreate -aey --mirrorlog mirrored --type mirror -m 2 --ignoremonitoring --nosync -L 1 -n 3way $vg \
    "$dev1" "$dev2" "$dev3" "$dev4":0 "$dev5":0
aux disable_dev "$dev4"
lvconvert -y --repair $vg/3way 2>&1 | tee 3way.out
lvs -a -o +devices $vg | not grep unknown
not grep "WARNING: Failed" 3way.out
vgreduce --removemissing $vg
check mirror $vg 3way
aux enable_dev "$dev4"
vgremove -ff $vg
