#!/usr/bin/env bash

# Copyright (C) 2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='devices file'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 1

# The tests run with system dir of "/etc" but lvm when running
# normally has cmd->system_dir set to "/etc/lvm".
DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"

aux lvmconf 'devices/use_devicesfile = 1' \
	    'devices/scan_lvs = 1'

rm -f "$DF"
touch "$DF"

vgcreate $vg1 "$dev1"
lvcreate -n $lv1 -L 8M $vg1
lvcreate -n $lv2 -L 8M $vg1
pvcreate "$DM_DEV_DIR/$vg1/$lv1"
pvcreate "$DM_DEV_DIR/$vg1/$lv2"
vgcreate $vg2 "$DM_DEV_DIR/$vg1/$lv1" "$DM_DEV_DIR/$vg1/$lv2"

pvs "$DM_DEV_DIR/$vg1/$lv1"
pvs "$DM_DEV_DIR/$vg1/$lv2"

grep "$dev1" "$DF"
grep "$DM_DEV_DIR/$vg1/$lv1" "$DF" | tee out1
grep "IDTYPE=lvmlv_uuid" out1
grep "IDNAME=LVM-" out1
grep "$DM_DEV_DIR/$vg1/$lv2" "$DF" | tee out2
grep "IDTYPE=lvmlv_uuid" out2
grep "IDNAME=LVM-" out2

lvremove -y $vg1/$lv1
not grep "$DM_DEV_DIR/$vg1/$lv1" "$DF"

lvremove -y $vg1/$lv2
not grep "$DM_DEV_DIR/$vg1/$lv2" "$DF"

grep "$dev1" "$DF"
not grep "IDTYPE=lvmlv_uuid" "$DF"
not grep "IDNAME=LVM-" "$DF"

vgremove -y $vg1
