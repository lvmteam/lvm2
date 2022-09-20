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

test_description='device id wwid from vpd_pg83'

. lib/inittest

test "$DM_DEV_DIR" = "/dev" || skip "Only works with /dev access -> make check LVM_TEST_DEVDIR=/dev"

aux lvmconf 'devices/use_devicesfile = 1'
# requires trailing / to match dm
aux lvmconf 'devices/device_id_sysfs_dir = "/test/sys/"'
SYS_DIR="/test/sys"

# These values match the values encoded in the binary blob
# written to dev1_vpd_pg83
DEV1_NAA=naa.600a098038303877413f4e7049592e6e
DEV1_EUI=eui.3f4e7049592d6f0000a0973730387741
DEV1_T10=t10.LVMTST_LUN_809wALVMTSTo
# dev has a second naa wwid
DEV1_NAA2=naa.600a098000000002ac18542400000dbd
# dev has a third naa wwid in the scsi name field
DEV1_NAA3=naa.553b13644430344b4e3f486d32647962

create_base() {
	mkdir -p $SYS_DIR/dev/block

	echo -n "0083 009c 0201 0020 4c56 4d54 5354 2020 \
	204c 554e 2038 3039 7741 4c56 4d54 5354 \
	6f20 2020 2020 2020 0103 0010 600a 0980 \
	3830 3877 413f 4e70 4959 2e6e 0102 0010 \
	3f4e 7049 592d 6f00 00a0 9737 3038 7741 \
	0113 0010 600a 0980 0000 0002 ac18 5424 \
	0000 0dbd 0114 0004 0101 0005 0115 0004 \
	0000 03ec 0328 0028 6e61 612e 3535 3342 \
	3133 3634 3434 3330 3334 3442 3445 3346 \
	3438 3644 3332 3634 3739 3632 0000 0000" | xxd -r -p > $SYS_DIR/dev1_vpd_pg83
}

remove_base() {
	rm $SYS_DIR/dev1_vpd_pg83
	rmdir $SYS_DIR/dev/block
	rmdir $SYS_DIR/dev
	rmdir $SYS_DIR
}

setup_sysfs() {
	mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
	echo $1 > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
	cp $SYS_DIR/dev1_vpd_pg83 $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/vpd_pg83
}

cleanup_sysfs() {
	rm -f $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
	rm -f $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/vpd_pg83
	rmdir $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
	rmdir $SYS_DIR/dev/block/$MAJOR1:$MINOR1
}


modprobe scsi_debug dev_size_mb=16 num_tgts=1
sleep 2
# Get scsi device name created by scsi_debug.
# SD = sdh
# DEV1 = /dev/sdh
SD=$(grep -H scsi_debug /sys/block/sd*/device/model | cut -f4 -d /);
echo $SD
DEV1=/dev/$SD
echo $DEV1

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"
touch $DF

pvcreate "$DEV1"
vgcreate $vg "$DEV1"
MAJOR1=`pvs "$DEV1" --noheading -o major | tr -d - | awk '{print $1}'`
MINOR1=`pvs "$DEV1" --noheading -o minor | tr -d - | awk '{print $1}'`
PVID1=`pvs "$DEV1" --noheading -o uuid | tr -d - | awk '{print $1}'`

create_base

# No sys/wwid, lvm uses wwid from sys/vpd

setup_sysfs $DEV1_NAA
# no sys/wwid is reported
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
rm $DF
lvmdevices --adddev "$DEV1"
cat $DF
pvs "$DEV1"
grep $DEV1_NAA $DF
cleanup_sysfs

# Kernel changes the type printed from sys/wwid from t10 to naa
# after lvm has used sys_wwid with the t10 value.
# set sys/wwid to t10 value
# add dev to df, it uses t10 value
# change sys/wwid to naa value
# reporting pvs should still find the dev based on using vpd data
#  and find the t10 value there

setup_sysfs $DEV1_T10
rm $DF
lvmdevices --adddev "$DEV1"
cat $DF
grep sys_wwid $DF
grep $DEV1_T10 $DF
pvs "$DEV1"
# kernel changes what it reports from sys/wwid
echo $DEV1_NAA > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
# lvm finds the original t10 id in vpd
pvs "$DEV1"
cleanup_sysfs

# User chooses wwid type other than is printed from sys/wwid
# set sys/wwid to t10|naa|eui value
# lvmdevices --adddev using --deviceidtype different from sys/wwid
# df entry uses the specified type
# reporting pvs should show the pv

setup_sysfs $DEV1_T10
rm $DF
lvmdevices --adddev "$DEV1" --deviceidtype wwid_naa
cat $DF
grep wwid_naa $DF
grep $DEV1_NAA $DF
pvs "$DEV1"
lvmdevices --deldev "$DEV1"
lvmdevices --addpvid "$PVID1" --deviceidtype wwid_naa
cat $DF
grep $DEV1_NAA $DF
pvs "$DEV1"
lvmdevices --deldev "$DEV1"
lvmdevices --adddev "$DEV1" --deviceidtype wwid_eui
cat $DF
grep wwid_eui $DF
grep $DEV1_EUI $DF
pvs "$DEV1"
cleanup_sysfs

# Any of the vpd wwids can be used in the devices file 
# with type sys_wwid and the device will be matched to
# it by finding that wwid in the vpd data.

setup_sysfs $DEV1_NAA
rm $DF
lvmdevices --adddev "$DEV1"
cat $DF
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
pvs "$DEV1"
cleanup_sysfs

setup_sysfs $DEV1_NAA2
rm $DF
lvmdevices --adddev "$DEV1"
cat $DF
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
pvs "$DEV1"
cleanup_sysfs

setup_sysfs $DEV1_NAA3
rm $DF
lvmdevices --adddev "$DEV1"
cat $DF
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
pvs "$DEV1"
cleanup_sysfs

setup_sysfs $DEV1_EUI
rm $DF
lvmdevices --adddev "$DEV1"
cat $DF
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
pvs "$DEV1"
cleanup_sysfs

setup_sysfs $DEV1_T10
rm $DF
lvmdevices --adddev "$DEV1"
cat $DF
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
pvs "$DEV1"
cleanup_sysfs


# TODO: lvmdevices --adddev <dev> --deviceidtype <type> --deviceid <val>
# This would let the user specify the second naa wwid.

remove_base
rmmod scsi_debug
