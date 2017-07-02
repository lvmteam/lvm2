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

#
# lvchange/vgchange -aay --sysinit should not activate LVs
# if lvmetad is configured and running.
#

vgcreate $vg1 "$dev1" "$dev2"
lvcreate -an -l1 --zero n -n $lv1 $vg1

#
# lvmetad is configured and running
#

lvchange -ay $vg1 2>&1 | tee out
not grep "WARNING: Failed to connect" out
not grep "WARNING: lvmetad is active, skipping direct activation during sysinit" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

lvchange -aay --sysinit $vg1 2>&1 | tee out
not grep "WARNING: Failed to connect" out
grep "WARNING: lvmetad is active, skipping direct activation during sysinit" out
check inactive $vg1 $lv1

lvchange -ay --sysinit $vg1 2>&1 | tee out
not grep "WARNING: Failed to connect" out
not grep "WARNING: lvmetad is active, skipping direct activation during sysinit" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

#
# lvmetad is configured and not running
#

kill "$(< LOCAL_LVMETAD)"

lvchange -ay $vg1 2>&1 | tee out
grep "WARNING: Failed to connect" out
not grep "WARNING: lvmetad is active, skipping direct activation during sysinit" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

lvchange -aay --sysinit $vg1 2>&1 | tee out
grep "WARNING: Failed to connect" out
not grep "WARNING: lvmetad is active, skipping direct activation during sysinit" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

#
# lvmetad is not configured and not running
#

aux lvmconf 'global/use_lvmetad = 0'

lvchange -ay $vg1 2>&1 | tee out
not grep "WARNING: Failed to connect" out
not grep "WARNING: lvmetad is active, skipping direct activation during sysinit" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

lvchange -aay $vg1 --sysinit 2>&1 | tee out
not grep "WARNING: Failed to connect"
not grep "WARNING: lvmetad is active, skipping direct activation during sysinit" out
check active $vg1 $lv1
lvchange -an $vg1
check inactive $vg1 $lv1

vgremove -ff $vg1
