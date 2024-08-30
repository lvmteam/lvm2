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

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

# Use local for this test vdo configuration
VDO_CONFIG="vdotestconf.yml"
VDOCONF="-f $VDO_CONFIG"
#VDOCONF=""
export VDOCONF VDO_CONFIG
VDONAME="${PREFIX}-TESTVDO"
export DM_UUID_PREFIX=$PREFIX

# Conversion can be made with this version of vdo driver
aux have_vdo 6 2 3 || skip

# With new upstream VDO conversion is not supported
aux have_vdo 9 0 0 && skip

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

# VDO automatically starts dmeventd
aux prepare_dmeventd

which mkfs.ext4 || skip
export MKE2FS_CONFIG="$TESTDIR/lib/mke2fs.conf"
export TMPDIR=$PWD

######################################################################
#
# !!! This is rather tricky way how to 'play' with large SCSI !!!
#
# Use 311/313MB 'wrap-around' RAM for 4GiB virtual disk.
# (using prime number for dev_size_mb)
#
# ATM this seems work with the test for layout of VDO written data.
# Minimal required size for VDO volume is ~4GiB
#
aux prepare_scsi_debug_dev 311 "virtual_gb=4"
SCSI_DEV=$(< SCSI_DEBUG_DEV)

# this non-scsi backend should always work, but we want to get DEVLINKS
#aux prepare_devs 1 6144
#SCSI_DEV=$dev1

blockdev --getsize64 "$SCSI_DEV"

aux extend_filter_LVMTEST "a|$SCSI_DEV|"

# use some not so 'well' aligned virtual|logical size
vdo create $VDOCONF --name "$VDONAME" --device "$SCSI_DEV" \
 --vdoSlabSize 128M --vdoLogicalSize 5M

mkfs.ext4 -E nodiscard "$DM_DEV_DIR/mapper/$VDONAME"
# Try just dry run and observe logging
lvm_import_vdo --dry-run -y -v --name $lv1 "$SCSI_DEV"

lvm_import_vdo -y --name $vg/$lv "$SCSI_DEV"
check lv_field $vg/$lv size "6.00m"

vgremove -f $vg
