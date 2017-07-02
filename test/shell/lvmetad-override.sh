#!/usr/bin/env bash

# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITHOUT_LVMETAD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_pvs 2

vgcreate $vg1 "$dev1" "$dev2"
lvcreate -an -l1 --zero n -n $lv1 $vg1

lvchange -ay $vg1 2>&1 | tee out
not grep "WARNING: Failed to connect" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

kill "$(< LOCAL_LVMETAD)"

lvchange -ay $vg1 2>&1 | tee out
grep "WARNING: Failed to connect" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

lvchange -ay --config global/use_lvmetad=0 $vg1 2>&1 | tee out
# FIXME: this warning appears when the command tries to connect to
# lvmetad during refresh at the end after the --config is cleared.
should not grep "WARNING: Failed to connect" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

aux lvmconf "global/use_lvmetad = 0"

lvchange -ay --config global/use_lvmetad=1 $vg1 2>&1 | tee out
grep "WARNING: Failed to connect" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

vgremove -ff $vg1
