#!/usr/bin/env bash

# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test conversion of VDO volumes made by vdo manager into VDO LV.


SKIP_WITH_LVMPOLLD=1

. lib/inittest

# Use local for this test vdo configuratoin
VDOCONF="-f vdotestconf.yml"
#VDOCONF=""
export VDOCONF
VDONAME="${PREFIX}-TESTVDO"

# VDO automatically starts dmeventd
aux prepare_dmeventd

#
# Main
#
which vdo || skip
which mkfs.ext4 || skip
export MKE2FS_CONFIG="$TESTDIR/lib/mke2fs.conf"

aux have_vdo 6 2 0 || skip

aux prepare_devs 2 10000

aux extend_filter_LVMTEST


#
#  Check conversion of VDO volume made on some LV
#
#  In this case we do not need to move any VDO headers.
#
vgcreate $vg "$dev1"

lvcreate -L5G -n $lv1 $vg

vdo create $VDOCONF --name "$VDONAME" --device="$DM_DEV_DIR/$vg/$lv1" --vdoLogicalSize=10G

mkfs -E nodiscard "$DM_DEV_DIR/mapper/$VDONAME"

# Different VG name fails
not lvm_import_vdo -y -v --name $vg1/$lv1 "$DM_DEV_DIR/$vg/$lv1"

# Try just dry run and observe logging
lvm_import_vdo --dry-run -y -v --name $lv1 "$DM_DEV_DIR/$vg/$lv1"

lvm_import_vdo -y --name $lv1 "$DM_DEV_DIR/$vg/$lv1"

# ATM needed - since we do not call 'vdo convert' in this case
vdo remove $VDOCONF --force --name "$VDONAME" || true

vgremove -f $vg

aux wipefs_a "$dev1"

# prepare 'unused' $vg2
vgcreate $vg2 "$dev2"

#
# Check conversion of VDO volume on  non-LV device
#
vdo create $VDOCONF --name "$VDONAME" --device="$dev1" --vdoLogicalSize=31G

mkfs -E nodiscard "$DM_DEV_DIR/mapper/$VDONAME"

# Fail with an already existing volume group $vg2
not lvm_import_vdo --dry-run -y -v --name $vg2/$lv1 "$dev1" |& tee err
grep "already existing volume group" err

# User can also convert already stopped VDO volume
vdo stop $VDOCONF --name "$VDONAME"

lvm_import_vdo -y -v --name $vg/$lv1 "$dev1"

fsck -n "$DM_DEV_DIR/$vg/$lv1"

vgremove -f $vg


#
# Try once again with different vgname/lvname and sizes
#
aux teardown_devs
aux prepare_devs 1 23456

vdo create $VDOCONF --name "$VDONAME" --device="$dev1" --vdoLogicalSize=23G

mkfs -E nodiscard "$DM_DEV_DIR/mapper/$VDONAME"

lvm_import_vdo -y -v --name $vg1/$lv2 "$dev1"

fsck -n "$DM_DEV_DIR/$vg1/$lv2"

vgremove -f $vg1

