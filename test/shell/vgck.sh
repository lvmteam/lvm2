#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

aux prepare_vg 3
lvcreate -n blabla -L 1 $vg

dd if=/dev/urandom bs=512 seek=2 count=32 of=$dev2

# TODO: aux lvmconf "global/locking_type = 4"

if test -e LOCAL_LVMETAD; then
    vgscan 2>&1 | tee vgscan.out
    not grep "Inconsistent metadata found for VG $vg" vgscan.out
else
    not vgscan 2>&1 | tee vgscan.out
    grep "Inconsistent metadata found for VG $vg" vgscan.out
fi

dd if=/dev/urandom bs=512 seek=2 count=32 of=$dev2
vgck $vg 2>&1 | tee vgck.out
grep Incorrect vgck.out
