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

SKIP_WITH_LVMPOLLD=1

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
	rmdir $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device || true
	rmdir $SYS_DIR/dev/block/$MAJOR1:$MINOR1 || true
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
DFTMP="$DFDIR/system.devices_tmp"
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

# Test nvme wwid that starts with "nvme" instead of naa/eui/t10
rm $DF
aux wipefs_a "$DEV1"
mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/
echo "nvme.111111111111111111122222222222333333333333333-44444444444444444445555555555556666666666666666662-00000001" > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/wwid
lvmdevices --adddev "$DEV1"
cat $DF
vgcreate $vg "$DEV1"
lvcreate -l1 -an $vg
cat $DF
pvs -o+deviceidtype,deviceid "$DEV1" |tee out
grep sys_wwid out
grep nvme.111 out
grep sys_wwid $DF
grep nvme.111 $DF
lvmdevices --deldev "$DEV1"
not lvmdevices --adddev "$DEV1" --deviceidtype wwid_eui
lvmdevices --adddev "$DEV1" --deviceidtype sys_wwid
lvmdevices | grep nvme.111
lvremove -y $vg
sleep 1
lvs $vg
vgremove $vg
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/wwid
cleanup_sysfs

# Test t10 wwid containing quote
rm $DF
aux wipefs_a "$DEV1"
mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
echo "t10.ATA_2.5\"_SATA_SSD_1112-A___111111111111" > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
lvmdevices --adddev "$DEV1"
cat $DF
vgcreate $vg "$DEV1"
lvcreate -l1 -an $vg
cat $DF
# check wwid string in metadata output
pvs -o+deviceidtype,deviceid "$DEV1" |tee out
grep sys_wwid out
# the quote is removed after the 5
grep 2.5_SATA_SSD out
# check wwid string in system.devices
grep sys_wwid $DF
# the quote is removed after the 5
grep 2.5_SATA_SSD $DF
lvremove -y $vg
vgremove $vg
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cleanup_sysfs

# Test t10 wwid with trailing space and line feed at the end
rm $DF
aux wipefs_a "$DEV1"
mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
echo -n "7431 302e 4154 4120 2020 2020 5642 4f58 \
2048 4152 4444 4953 4b20 2020 2020 2020 \
2020 2020 2020 2020 2020 2020 2020 2020 \
2020 2020 5642 3963 3130 6433 3138 2d31 \
3838 6439 6562 6320 0a" | xxd -r -p > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cat $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
lvmdevices --adddev "$DEV1"
cat $DF
vgcreate $vg "$DEV1"
lvcreate -l1 -an $vg
cat $DF
# check wwid string in metadata output
pvs -o+deviceidtype,deviceid "$DEV1" |tee out
grep sys_wwid out
# check wwid string in system.devices
grep sys_wwid $DF
lvremove -y $vg
vgremove $vg
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cleanup_sysfs

# Test t10 wwid with trailing space at the end that was created by 9.0/9.1
rm $DF
aux wipefs_a "$DEV1"
mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
echo -n "7431 302e 4154 4120 2020 2020 5642 4f58 \
2048 4152 4444 4953 4b20 2020 2020 2020 \
2020 2020 2020 2020 2020 2020 2020 2020 \
2020 2020 5642 3963 3130 6433 3138 2d31 \
3838 6439 6562 6320 0a" | xxd -r -p > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cat $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
lvmdevices --adddev "$DEV1"
cat $DF
vgcreate $vg "$DEV1"
PVID1=`pvs "$DEV1" --noheading -o uuid | tr -d - | awk '{print $1}'`
T10_WWID_RHEL91="t10.ATA_____VBOX_HARDDISK___________________________VB9c10d318-188d9ebc_"
lvcreate -l1 -an $vg
cat $DF
# check wwid string in metadata output
pvs -o+deviceidtype,deviceid "$DEV1" |tee out
grep sys_wwid out
# check wwid string in system.devices
grep sys_wwid $DF
# Replace IDNAME with the IDNAME that 9.0/9.1 created from this wwid
cat $DF | grep -v IDNAME > $DFTMP
cat $DFTMP
echo "IDTYPE=sys_wwid IDNAME=t10.ATA_____VBOX_HARDDISK___________________________VB9c10d318-188d9ebc_ DEVNAME=${DEV1} PVID=${PVID1}" >> $DFTMP
cp $DFTMP $DF
cat $DF
vgs
pvs
pvs -o+deviceidtype,deviceid "$DEV1"
# Removing the trailing _ which should then work
cat $DF | grep -v IDNAME > $DFTMP
cat $DFTMP
echo "IDTYPE=sys_wwid IDNAME=t10.ATA_____VBOX_HARDDISK___________________________VB9c10d318-188d9ebc DEVNAME=${DEV1} PVID=${PVID1}" >> $DFTMP
cp $DFTMP $DF
cat $DF
vgs
pvs
pvs -o+deviceidtype,deviceid "$DEV1"
lvremove -y $vg
vgremove $vg
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cleanup_sysfs

# test a t10 wwid that has actual trailing underscore which
# is followed by a trailing space.
rm $DF
aux wipefs_a "$DEV1"
mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
echo -n "7431 302e 4154 4120 2020 2020 5642 4f58 \
2048 4152 4444 4953 4b20 2020 2020 2020 \
2020 2020 2020 2020 2020 2020 2020 2020 \
2020 2020 5642 3963 3130 6433 3138 2d31 \
3838 6439 6562 5f20 0a" | xxd -r -p > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cat $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
# The wwid has an actual underscore char (5f) followed by a space char (20)
# 9.1 converts the trailing space to an underscore
T10_WWID_RHEL91="t10.ATA_____VBOX_HARDDISK___________________________VB9c10d318-188d9eb__"
# 9.2 ignores the trailing space
T10_WWID_RHEL92="t10.ATA_____VBOX_HARDDISK___________________________VB9c10d318-188d9eb_"
lvmdevices --adddev "$DEV1"
cat $DF
vgcreate $vg "$DEV1"
PVID1=`pvs "$DEV1" --noheading -o uuid | tr -d - | awk '{print $1}'`
lvcreate -l1 -an $vg
cat $DF
# check wwid string in metadata output
pvs -o+deviceidtype,deviceid "$DEV1" |tee out
grep sys_wwid out
# check wwid string in system.devices
grep sys_wwid $DF
# Replace IDNAME with the IDNAME that 9.0/9.1 created from this wwid
cat $DF | grep -v IDNAME > $DFTMP
cat $DFTMP
echo "IDTYPE=sys_wwid IDNAME=${T10_WWID_RHEL91} DEVNAME=${DEV1} PVID=${PVID1}" >> $DFTMP
cp $DFTMP $DF
cat $DF
vgs
pvs
pvs -o+deviceidtype,deviceid "$DEV1"
lvremove -y $vg
vgremove $vg
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cleanup_sysfs

#
# Test trailing/leading/center spaces in sys_wwid and sys_serial device
# ids, and that old system.devices files that have trailing/leading
# underscores are understood.
#

rm $DF
aux wipefs_a "$DEV1"
mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
echo -n "  s123  456  " > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial
lvmdevices --adddev "$DEV1"
cat $DF
grep "IDNAME=s123__456 DEVNAME" $DF
vgcreate $vg "$DEV1"
PVID1=`pvs "$DEV1" --noheading -o uuid | tr -d - | awk '{print $1}'`
cat $DF | grep -v IDNAME > $DFTMP
cat $DFTMP
echo "IDTYPE=sys_serial IDNAME=__s123__456__ DEVNAME=${DEV1} PVID=${PVID1}" >> $DFTMP
cp $DFTMP $DF
cat $DF
vgs
pvs -o+deviceidtype,deviceid "$DEV1"
lvremove -y $vg
vgremove $vg
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial
cleanup_sysfs

rm $DF
aux wipefs_a "$DEV1"
mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
echo -n "  t10.123  456  " > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
lvmdevices --adddev "$DEV1"
cat $DF
grep "IDNAME=t10.123_456 DEVNAME" $DF
vgcreate $vg "$DEV1"
PVID1=`pvs "$DEV1" --noheading -o uuid | tr -d - | awk '{print $1}'`
cat $DF | grep -v IDNAME > $DFTMP
cat $DFTMP
echo "IDTYPE=sys_wwid IDNAME=__t10.123__456__ DEVNAME=${DEV1} PVID=${PVID1}" >> $DFTMP
cp $DFTMP $DF
cat $DF
vgs
pvs -o+deviceidtype,deviceid "$DEV1"
lvremove -y $vg
vgremove $vg
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cleanup_sysfs

rm $DF
aux wipefs_a "$DEV1"
mkdir -p $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device
echo -n "  naa.123  456  " > $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
lvmdevices --adddev "$DEV1"
cat $DF
grep "IDNAME=naa.123__456 DEVNAME" $DF
vgcreate $vg "$DEV1"
PVID1=`pvs "$DEV1" --noheading -o uuid | tr -d - | awk '{print $1}'`
cat $DF | grep -v IDNAME > $DFTMP
cat $DFTMP
echo "IDTYPE=sys_wwid IDNAME=__naa.123__456__ DEVNAME=${DEV1} PVID=${PVID1}" >> $DFTMP
cp $DFTMP $DF
cat $DF
vgs
pvs -o+deviceidtype,deviceid "$DEV1"
lvremove -y $vg
vgremove $vg
rm $SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid
cleanup_sysfs




# TODO: lvmdevices --adddev <dev> --deviceidtype <type> --deviceid <val>
# This would let the user specify the second naa wwid.

remove_base
rmmod scsi_debug || true
