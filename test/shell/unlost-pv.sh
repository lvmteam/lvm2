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

check_() {
	# vgscan needs --cache option for direct scan if lvmetad is used
	test -e LOCAL_LVMETAD && cache="--cache"
	vgscan $cache 2>&1 | tee vgscan.out
	$1 grep "Inconsistent metadata found for VG $vg" vgscan.out
}

aux prepare_vg 3

lvcreate -an -Zn --type mirror -m 1 -l 1 -n mirror $vg
#lvchange -a n $vg

# try orphaning a missing PV (bz45867)
aux disable_dev "$dev1"
vgreduce --removemissing --force $vg
aux enable_dev "$dev1"

check_
test -e LOCAL_LVMETAD && pvcreate -f "$dev1"
check_ not

# try to just change metadata; we expect the new version (with MISSING_PV set
# on the reappeared volume) to be written out to the previously missing PV
vgextend $vg "$dev1"
lvcreate -l 1 -n boo -a n --zero n $vg
aux disable_dev "$dev1"
lvremove $vg/mirror
aux enable_dev "$dev1"
check_
test -e LOCAL_LVMETAD && lvremove $vg/boo # FIXME trigger a write :-(
check_ not
