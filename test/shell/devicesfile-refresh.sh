#!/usr/bin/env bash

# Copyright (C) 2020-23 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='refresh device ids if system changes'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

test -d /sys/block/ram0 && skip "Ramdisk already loaded"

test "$DM_DEV_DIR" = "/dev" || skip "Only works with /dev access -> make check LVM_TEST_DEVDIR=/dev"

# requires trailing / to match dm
SYS_DIR="$PWD/test/sys"
aux lvmconf "devices/use_devicesfile = 1" \
	"devices/device_id_sysfs_dir = \"$SYS_DIR/\"" \
	'devices/global_filter = [ "a|.*|" ]' \
	"global/event_activation = 1"

SERIAL1="S111"
SERIAL2="S222"
SERIAL3="S333"
SERIAL4="S444"

PRODUCT_UUID1="11111111-2222-3333-4444-555555555555"
PRODUCT_UUID2="11111111-2222-3333-4444-666666666666"

create_sysfs() {
	mkdir -p "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device"

	echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
	echo "$SERIAL2" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
	echo "$SERIAL3" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
	echo "$SERIAL4" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

	mkdir -p "$SYS_DIR/devices/virtual/dmi/id/"
	echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
}

remove_sysfs() {
	rm -rf "$SYS_DIR"
}

cleanup_and_teardown()
{
	remove_sysfs
	rmmod brd

	aux teardown
}

trap 'cleanup_and_teardown' EXIT

modprobe brd rd_nr=4  || skip
sleep 1
remove_sysfs

dev1="/dev/ram0"
dev2="/dev/ram1"
dev3="/dev/ram2"
dev4="/dev/ram3"

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"
ORIG="$DFDIR/orig.devices"
touch "$DF"

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev3"
aux wipefs_a "$dev4"

vgcreate $vg1 "$dev1"
eval "$(pvs --noheading --nameprefixes -o major,minor,uuid "$dev1")"
MAJOR1=$LVM2_PV_MAJOR
MINOR1=$LVM2_PV_MINOR
OPVID1=$LVM2_PV_UUID
PVID1=${OPVID1//-/}

vgcreate $vg2 "$dev2"
eval "$(pvs --noheading --nameprefixes -o major,minor,uuid "$dev2")"
MAJOR2=$LVM2_PV_MAJOR
MINOR2=$LVM2_PV_MINOR
OPVID2=$LVM2_PV_UUID
PVID2=${OPVID2//-/}

# just using pvcreate/pvs to get MAJOR MINOR

pvcreate "$dev3"
eval "$(pvs --noheading --nameprefixes -o major,minor,uuid "$dev3")"
MAJOR3=$LVM2_PV_MAJOR
MINOR3=$LVM2_PV_MINOR

pvcreate "$dev4"
eval "$(pvs --noheading --nameprefixes -o major,minor,uuid "$dev4")"
MAJOR4=$LVM2_PV_MAJOR
MINOR4=$LVM2_PV_MINOR

pvremove "$dev3"
pvremove "$dev4"
aux wipefs_a "$dev3"
aux wipefs_a "$dev4"

create_sysfs

rm "$DF"

vgimportdevices $vg1
vgimportdevices $vg2

cat "$DF"

grep $PRODUCT_UUID1 "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
not grep "$dev3" "$DF"
not grep "$dev4" "$DF"
grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
not grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

pvs |tee out
grep "$dev1" out
grep "$dev2" out
not grep "$dev3" out
not grep "$dev4" out

# Prints the deviceid that's saved in metadata.
pvs -o uuid,deviceid "$dev1" | tee out
grep $OPVID1 out
grep $SERIAL1 out

# PV1 moves from dev1 to dev3 (and dev1 goes away)
# lvm does not find PV1 until the product_uuid changes which
# triggers the command to look at devs outside the DF.

# PV1 moves to new dev
dd if="$dev1" of="$dev3" bs=1M count=1
aux wipefs_a "$dev1"
rm "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"

# PV1 not found
pvs |tee out
not grep "$dev1" out
grep "$dev2" out
not grep "$dev3" out
not grep "$dev4" out

# DF unchanged
grep $PRODUCT_UUID1 "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
not grep "$dev3" "$DF"
not grep "$dev4" "$DF"
grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
not grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

# product_uuid changes
echo "$PRODUCT_UUID2" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"

# PV1 found on new dev
pvs |tee out
not grep "$dev1" out
grep "$dev2" out
grep "$dev3" out
not grep "$dev4" out

# DF updated replacing old dev with new dev
grep $PRODUCT_UUID2 "$DF"
not grep "$dev1" "$DF"
grep "$dev2" "$DF"
grep "$dev3" "$DF"
not grep "$dev4" "$DF"
not grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

# PV1 was originally written to dev1 but has not
# moved to dev3.  The deviceid in the metadata is
# S111 from dev1, but the PV is now on dev3 which
# has deviceid S333.  Since the deviceid of the dev
# doesn't match the deviceid savedin metadata,
# "invalid" is printed when displaying the outdated
# deviceid from the metadata.
pvs -o uuid,deviceid "$dev3" | tee out
grep $OPVID1 out
grep invalid out

# bring back dev1
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"


# No product_uuid so hostname is used

rm "$SYS_DIR/devices/virtual/dmi/id/product_uuid"

rm "$DF"
vgimportdevices $vg1
vgimportdevices $vg2

grep HOSTNAME "$DF"
not grep PRODUCT_UUID "$DF"

pvs |tee out
not grep "$dev1" out
grep "$dev2" out
grep "$dev3" out
not grep "$dev4" out

not grep "$dev1" "$DF"
grep "$dev2" "$DF"
grep "$dev3" "$DF"
not grep "$dev4" "$DF"

# PV1 moves from dev3 back to dev1
# lvm does not find PV1 until the hostname changes which
# triggers the command to look at devs outside the DF.

# PV1 moves to new dev
dd if="$dev3" of="$dev1" bs=1M count=1
aux wipefs_a "$dev3"
rm "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"

# PV1 not found
pvs |tee out
not grep "$dev1" out
grep "$dev2" out
not grep "$dev3" out
not grep "$dev4" out

# we can't change the hostname to trigger lvm refresh,
# but removing the HOSTNAME line from system.devices
# will be a trigger.
sed -e "s|HOSTNAME=.||" "$DF" > tmpdf
cp tmpdf "$DF"

# PV1 found on new dev
pvs |tee out
grep "$dev1" out
grep "$dev2" out
not grep "$dev3" out
not grep "$dev4" out

# DF updated replacing old dev with new dev
not grep PRODUCT_UUID "$DF"
grep HOSTNAME "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
not grep "$dev3" "$DF"
not grep "$dev4" "$DF"
grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
not grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

# bring back dev3
echo "$SERIAL3" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"

# DF has no PRODUCT_UUID or HOSTNAME, lvm command adds one

rm "$DF"
vgimportdevices $vg1
vgimportdevices $vg2

sed -e "s|HOSTNAME=.||" "$DF" > tmpdf
cp tmpdf "$DF"
sed -e "s|PRODUCT_UUID=.||" "$DF" > tmpdf
cp tmpdf "$DF"

not grep HOSTNAME "$DF"
not grep PRODUCT_UUID "$DF"

pvs
grep HOSTNAME "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
not grep "$dev3" "$DF"
not grep "$dev4" "$DF"
grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
not grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"


# DF has PRODUCT_UUID but system only has hostname,
# and PV1 moves to different device

echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
rm "$DF"
vgimportdevices $vg1
vgimportdevices $vg2
rm "$SYS_DIR/devices/virtual/dmi/id/product_uuid"

# PV1 moves from dev1 to dev3
dd if="$dev1" of="$dev3" bs=1M count=1
aux wipefs_a "$dev1"
rm "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"

pvs
grep HOSTNAME "$DF"
not grep "$dev1" "$DF"
grep "$dev2" "$DF"
grep "$dev3" "$DF"
not grep "$dev4" "$DF"
not grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

# bring back dev1
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"

# DF has HOSTNAME but system has product_uuid, lvm command updates it

rm "$DF"
vgimportdevices $vg1
vgimportdevices $vg2

grep HOSTNAME "$DF"
echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
pvs
grep "$PRODUCT_UUID1" "$DF"
not grep HOSTNAME "$DF"


# DF has PRODUCT_UUID, system product_uuid changes, lvm command updates it

rm "$DF"
vgimportdevices $vg1
vgimportdevices $vg2

grep "$PRODUCT_UUID1" "$DF"
echo "$PRODUCT_UUID2" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
pvs
grep "$PRODUCT_UUID2" "$DF"

# PV1 moves from dev3 back to dev1
dd if="$dev3" of="$dev1" bs=1M count=1
aux wipefs_a "$dev3"


#
# pvscan --cache and vgchange -aay work when refresh is triggered and
# the device ids are wrong on the PVs that need to be autoactivated.
#

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
PVS_ONLINE_DIR="$RUNDIR/lvm/pvs_online"
VGS_ONLINE_DIR="$RUNDIR/lvm/vgs_online"
PVS_LOOKUP_DIR="$RUNDIR/lvm/pvs_lookup"

_clear_online_files() {
        # wait till udev is finished
        aux udev_wait
        rm -f "$PVS_ONLINE_DIR"/*
        rm -f "$VGS_ONLINE_DIR"/*
        rm -f "$PVS_LOOKUP_DIR"/*
}

echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
rm "$DF"
vgimportdevices $vg1
vgimportdevices $vg2
grep "$PRODUCT_UUID1" "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
lvcreate -l1 -an -n $lv1 $vg1
lvcreate -l1 -an -n $lv1 $vg2
pvs -o+deviceid

# PV1 moves from dev1 to dev3
dd if="$dev1" of="$dev3" bs=1M count=1
aux wipefs_a "$dev1"
rm "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"

_clear_online_files

# One PV in VG to autoactivate when system.devices has the wrong device ID
# PV1 is listed in system.devices as being from dev1 with SERIAL1,
# but PV1 is actually appearing from dev3 with SERIAL3.  PRODUCT_UUID is
# wrong, so refresh is triggered and PV1 will be used from dev3.

echo "$PRODUCT_UUID2" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event "$dev3"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/vgs_online/$vg1"
vgchange -aay --autoactivation event $vg1

# DF should be unchanged and have old info since the event based pvscan
# and vgchange are special/optimized for auto activation and don't update DF
grep "$PRODUCT_UUID1" "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
not grep "$dev3" "$DF"
not grep "$dev4" "$DF"
grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
not grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

# check that pvs will update DF PV1 to have SERIAL3
pvs
grep "$PRODUCT_UUID2" "$DF"
not grep "$dev1" "$DF"
grep "$dev2" "$DF"
grep "$dev3" "$DF"
not grep "$dev4" "$DF"
not grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

# check that the vgchange aay above actually activated the LV
lvs -o active $vg1/$lv1 | grep active

vgchange -an $vg1

# bring back dev1
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"

# Two PVs in VG to autoactivate when system.devices has the wrong device ID

# PV1 moves from dev3 back to dev1
dd if="$dev3" of="$dev1" bs=1M count=1
aux wipefs_a "$dev3"

rm "$DF"
vgremove -ff $vg1
vgremove -ff $vg2
pvs

echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"

vgcreate $vg1 "$dev1" "$dev2"
vgimportdevices $vg1
lvcreate -l1 -n $lv1 $vg1 "$dev1"
lvcreate -l1 -n $lv2 $vg1 "$dev2"
lvcreate -l4 -i2 -n $lv3 $vg1 "$dev1" "$dev2"
vgchange -an $vg1

grep "$PRODUCT_UUID1" "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
not grep "$dev3" "$DF"
not grep "$dev4" "$DF"
grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
not grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

# PV1 moves from dev1 to dev3
dd if="$dev1" of="$dev3" bs=1M count=1
aux wipefs_a "$dev1"
rm "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"

# PV2 moves from dev2 to dev4
dd if="$dev2" of="$dev4" bs=1M count=1
aux wipefs_a "$dev2"
rm "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"

_clear_online_files

# Two PVs in VG to autoactivate when system.devices has the wrong device ID
# system.devices says PV1 has SERIAL1 and PV2 has SERIAL2, but the new
# system has PV1 on SERIAL3 and PV2 on SERIAL4.
# PRODUCT_UUID is wrong, so refresh finds PV1/PV2 on SERIAL3/SERIAL4

echo "$PRODUCT_UUID2" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event "$dev3"
pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event "$dev4"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/vgs_online/$vg1"
ls "$RUNDIR/lvm/pvs_lookup/$vg1"
vgchange -aay --autoactivation event $vg1

# DF not yet updated by pvscan/vgchange

grep "$PRODUCT_UUID1" "$DF"
grep "$dev1" "$DF"
grep "$dev2" "$DF"
not grep "$dev3" "$DF"
not grep "$dev4" "$DF"
grep $SERIAL1 "$DF"
grep $SERIAL2 "$DF"
not grep $SERIAL3 "$DF"
not grep $SERIAL4 "$DF"

# check that lvmdevices will update DF
lvmdevices --update
grep "$PRODUCT_UUID2" "$DF"
not grep "$dev1" "$DF"
not grep "$dev2" "$DF"
grep "$dev3" "$DF"
grep "$dev4" "$DF"
not grep $SERIAL1 "$DF"
not grep $SERIAL2 "$DF"
grep $SERIAL3 "$DF"
grep $SERIAL4 "$DF"

# check that the vgchange actually activated LVs
lvs $vg1
lvs -o active $vg1/$lv1 | grep active
lvs -o active $vg1/$lv2 | grep active
lvs -o active $vg1/$lv3 | grep active

vgchange -an $vg1
vgremove -ff $vg1

