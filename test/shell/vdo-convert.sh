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

# Use local for this test vdo configuration
VDO_CONFIG="vdotestconf.yml"
VDOCONF="-f $VDO_CONFIG"
#VDOCONF=""
export VDOCONF VDO_CONFIG
VDONAME="${PREFIX}-TESTVDO"
export DM_UUID_PREFIX=$PREFIX

# VDO automatically starts dmeventd
aux prepare_dmeventd

#
# Main
#
if not which vdo ; then
	which lvm_vdo_wrapper || skip "Missing 'lvm_vdo_wrapper'."
	which oldvdoformat || skip "Emulation of vdo manager 'oldvdoformat' missing."
	which oldvdoprepareforlvm || skip "Emulation of vdo manager 'oldvdoprepareforlvm' missing."
	# enable expansion of aliasis within script itself
	shopt -s expand_aliases
	alias vdo='lvm_vdo_wrapper'
	export VDO_BINARY=lvm_vdo_wrapper
	echo "Using 'lvm_vdo_wrapper' emulation of 'vdo' manager."
fi
which mkfs.ext4 || skip
export MKE2FS_CONFIG="$TESTDIR/lib/mke2fs.conf"

# Conversion can be made with this version of vdo driver
aux have_vdo 6 2 3 || skip

# With new upstream VDO conversion is not supported
aux have_vdo 9 0 0 && skip

aux prepare_devs 2 20000

aux extend_filter_LVMTEST

export TMPDIR=$PWD


#
#  Check conversion of VDO volume made on some LV
#
#  In this case we do not need to move any VDO headers.
#
vgcreate $vg "$dev1"

lvcreate -L5G -n $lv1 $vg

# use some not so 'well' aligned virtual|logical size
vdo create $VDOCONF --name "$VDONAME" --device "$DM_DEV_DIR/$vg/$lv1" --vdoSlabSize 128M --vdoLogicalSize 10G

mkfs -E nodiscard "$DM_DEV_DIR/mapper/$VDONAME"
##XXXXX
# Different VG name fails
not lvm_import_vdo -y -v --name $vg1/$lv1 "$DM_DEV_DIR/$vg/$lv1"

# Try just dry run and observe logging
lvm_import_vdo --dry-run -y -v --name $lv1 "$DM_DEV_DIR/$vg/$lv1"

lvm_import_vdo -y --name $lv1 "$DM_DEV_DIR/$vg/$lv1"

# ensure VDO device is not left in config file
vdo remove $VDOCONF --force --name "$VDONAME" 2>/dev/null || true

lvremove -f $vg


# Test user can specify different VDO LV name (so the original LV is renamed)
lvcreate -y -L5G -n $lv1 $vg

vdo create $VDOCONF --name "$VDONAME" --device "$DM_DEV_DIR/$vg/$lv1" --vdoSlabSize 128M --vdoLogicalSize 10G

lvm_import_vdo -y --name $vg/$lv2 "$DM_DEV_DIR/$vg/$lv1"

check lv_exists $vg $lv2
check lv_not_exists $vg $lv1

vgremove -f $vg

# ensure VDO device is not left in config file
vdo remove $VDOCONF --force --name "$VDONAME" 2>/dev/null || true

aux wipefs_a "$dev1"

# prepare 'unused' $vg2
vgcreate $vg2 "$dev2"

#
# Check conversion of VDO volume on non-LV device and with >2T size
#
vdo create $VDOCONF --name "$VDONAME" --device "$dev1" --vdoSlabSize 128M --vdoLogicalSize 3T

# Fail with an already existing volume group $vg2
not lvm_import_vdo --dry-run -y -v --name $vg2/$lv1 "$dev1" |& tee err
grep "already existing volume group" err

# User can also convert already stopped VDO volume
vdo stop $VDOCONF --name "$VDONAME"

lvm_import_vdo -y -v --name $vg/$lv1 "$dev1"

check lv_field $vg/$lv1 size "3.00t"

vgremove -f $vg


#
# Try once again with different vgname/lvname and sizes
#
aux teardown_devs
aux prepare_devs 1 23456

vdo create $VDOCONF --name "$VDONAME" --device "$dev1" --vdoSlabSize 128M --vdoLogicalSize 23G

mkfs -E nodiscard "$DM_DEV_DIR/mapper/$VDONAME"

lvm_import_vdo --vdo-config "$VDO_CONFIG" -y -v --name $vg1/$lv2 "$dev1"

fsck -n "$DM_DEV_DIR/$vg1/$lv2"

vgremove -f $vg1



########################################################################
#
# Preparation of already moved header works only with fake vdo wrapper
#
########################################################################
test "${VDO_BINARY-}" != "lvm_vdo_wrapper" && exit

aux wipefs_a "$dev1"

# let's assume users with VDO target have 'new' enough version of stat too
# otherwise use more universal code from lvm_vdo_import
read major minor < <(stat -c '%Hr %Lr' $(readlink -e "$dev1"))
dmsetup create "$PREFIX-vdotest" --table "0 30280004 linear $major:$minor 32"

TEST="$DM_DEV_DIR/mapper/$PREFIX-vdotest"

aux wipefs_a "$TEST"
aux extend_filter "a|$TEST|"
aux extend_devices "$TEST"

#
# Unfortunately generates this in syslog:
#
# vdo-start-by-dev@loop0.service: Main process exited, code=exited, status=1/FAILURE
# vdo-start-by-dev@loop0.service: Failed with result 'exit-code'.
# Failed to start Start VDO volume backed by loop0.
#
# TODO:  Could be handled by:
#
# systemctl mask vdo-start-by-dev@
# systemctl unmask vdo-start-by-dev@
#
# automate...
#

# use slightly smaller size then 'rounded' 23G - to enforce vdo_logicalSize rounding
vdo create $VDOCONF --name "$VDONAME" --device "$TEST" --vdoSlabSize 128M --vdoLogicalSize 24117240K\
	--blockMapCacheSize 192 \
	--blockMapPeriod 2048 \
	--emulate512 disabled \
	--indexMem 0.5 \
	--maxDiscardSize 10 \
	--sparseIndex disabled \
	--vdoAckThreads 2 \
	--vdoBioRotationInterval 8 \
	--vdoBioThreads 2 \
	--vdoCpuThreads 5 \
	--vdoHashZoneThreads 3 \
	--vdoLogicalThreads 3 \
	--writePolicy async-unsafe
dmsetup table

# Get VDO table line
dmsetup table "$VDONAME" | tr " " "\n" | sed -e '5,6d' -e '12d' | tee vdo-orig

mkfs.ext4 -E nodiscard "$DM_DEV_DIR/mapper/$VDONAME"
rm -f debug.log*

# For the easy table validation of conversion we use old version4 format
aux lvmconf 'global/vdo_disabled_features = [ "version4" ]'

#
# Try to prepare 'broken' case where header was moved by older tool to 2M position
#
export LVM_VDO_PREPARE=oldvdoprepareforlvm2M
if which "$LVM_VDO_PREPARE" ; then
# Use old vdoprepareforlvm tool, that always moves header to 2M offset
cp "$VDO_CONFIG" "$VDO_CONFIG.backup"
lvm_import_vdo --abort-after-vdo-convert --vdo-config "$VDO_CONFIG" -v -y --name $vg/$lv "$TEST"
rm -f debug.log*
# Restore VDO configuration (as it's been removed with successful vdo conversion
cp "$VDO_CONFIG.backup" "$VDO_CONFIG"
# Check VDO header is seen at 2M offset
blkid -c /dev/null --probe --offset 2M "$TEST" || die "VDO header at unknown offset, expected 2M!"
fi

unset LVM_VDO_PREPARE

#lvm_import_vdo --no-snapshot --vdo-config "$VDO_CONFIG" -v -y --name $vg/$lv "$TEST"
lvm_import_vdo --vdo-config "$VDO_CONFIG" --uuid-prefix "$PREFIX" -v -y --name $vg/$lv "$TEST"
dmsetup table

# check our filesystem is OK
fsck -n "$DM_DEV_DIR/$vg/$lv"

# Compare converted LV uses same VDO table line
dmsetup table "$vg-${lv}_vpool-vpool" | tr " " "\n" | sed -e '5,6d' -e '12d' | tee new-vdo-lv

tail -n+3 vdo-orig >vdo-orig-3
tail -n+3 new-vdo-lv >new-vdo-lv-3

# Check there is a match between VDO and LV managed volume
# (when differentiating parameters are deleted first)
# we need to skip first 2 lines as the device size gets rounded to match VG extent size
diff -u vdo-orig-3 new-vdo-lv-3 || die "Found mismatching VDO table lines!"

check lv_field $vg/$lv size "23.00g"

vgremove -f $vg
