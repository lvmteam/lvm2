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

test_description='device id tests'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

test -d /sys/block/ram0 && skip "Ramdisk already loaded"

test "$DM_DEV_DIR" = "/dev" || skip "Only works with /dev access -> make check LVM_TEST_DEVDIR=/dev"


RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
HINTS="$RUNDIR/lvm/hints"

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

aux lvmconf 'devices/global_filter = [ "a|.*|" ]' \
            'devices/filter = [ "a|.*|" ]'

# requires trailing / to match dm
SYS_DIR="$PWD/test/sys"
aux lvmconf "devices/use_devicesfile = 1" \
	"devices/device_id_sysfs_dir = \"$SYS_DIR/\""

WWID1="naa.123456"
WWID2="nvme.123-456"

create_base() {
	mkdir -p "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device"

	echo "$WWID1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid"
	echo "$WWID2" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/wwid"
}

remove_base() {
	rm -rf "$SYS_DIR"
}

cleanup_and_teardown()
{
	vgremove -ff $vg1 || true
	remove_base
	rmmod brd

	aux teardown
}

trap 'cleanup_and_teardown' EXIT

modprobe brd rd_nr=4  || skip
sleep 1
remove_base

dev1="/dev/ram0"
dev2="/dev/ram1"
dev3="/dev/ram2"
dev4="/dev/ram3"

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"
touch $DF

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

vgcreate $vg3 "$dev3"
eval "$(pvs --noheading --nameprefixes -o major,minor,uuid "$dev3")"
MAJOR3=$LVM2_PV_MAJOR
MINOR3=$LVM2_PV_MINOR
OPVID3=$LVM2_PV_UUID
PVID3=${OPVID3//-/}

vgcreate $vg4 "$dev4"
eval "$(pvs --noheading --nameprefixes -o major,minor,uuid "$dev4")"
MAJOR4=$LVM2_PV_MAJOR
MINOR4=$LVM2_PV_MINOR
OPVID4=$LVM2_PV_UUID
PVID4=${OPVID4//-/}

create_base

# dev3 (without wwid) is listed before dev1 (with wwid), and they swap names
# pvs handles it

rm $DF
lvmdevices --adddev "$dev3"
lvmdevices --adddev "$dev2"
lvmdevices --adddev "$dev1"
cat $DF

cp "$DF" orig
sed -e "s|DEVNAME=$dev1|DEVNAME=tmpnm|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=$dev1|" tmp1 > tmp2
sed -e "s|IDNAME=$dev3|IDNAME=$dev1|" tmp2 > tmp3
sed -e "s|DEVNAME=tmpnm|DEVNAME=$dev3|" tmp3 > $DF
cat "$DF"

pvs -o+uuid |tee out

grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$dev3" out |tee out3
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3

grep "$PVID1" "$DF" |tee out
grep "$WWID1" out
grep "DEVNAME=$dev1" out

grep "$PVID3" "$DF" |tee out
not grep "$WWID1" out
grep "IDNAME=$dev3" out
grep "DEVNAME=$dev3" out

# dev3 (without wwid) is listed before dev1 (with wwid), and they swap names
# pvscan --cache dev handles it

rm "$DF"
vgimportdevices -a

vgremove $vg1 $vg2 $vg3 $vg4
vgcreate $vg1 "$dev1" "$dev2" "$dev3" "$dev4"
lvcreate -an -n $lv1 -l1 $vg1

rm $DF
lvmdevices --adddev "$dev4"
lvmdevices --adddev "$dev3"
lvmdevices --adddev "$dev2"
lvmdevices --adddev "$dev1"
cat $DF

cp "$DF" orig
sed -e "s|DEVNAME=$dev1|DEVNAME=tmpnm|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=$dev1|" tmp1 > tmp2
sed -e "s|IDNAME=$dev3|IDNAME=$dev1|" tmp2 > tmp3
sed -e "s|DEVNAME=tmpnm|DEVNAME=$dev3|" tmp3 > $DF
cat "$DF"

_clear_online_files

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event "$dev1"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
not ls "$RUNDIR/lvm/vgs_online/$vg1"

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event "$dev2"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
not ls "$RUNDIR/lvm/vgs_online/$vg1"

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event "$dev3"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
not ls "$RUNDIR/lvm/vgs_online/$vg1"

pvscan --cache --listvg --checkcomplete --vgonline --autoactivation event "$dev4"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/vgs_online/$vg1"

cat $DF

vgchange -aay --autoactivation event $vg1

cat $DF

# pvs will fix the DF entries
# (pvscan and vgchange aay skip the update to avoid interfering
# with the autoactivation process.)
pvs -o+uuid |tee out

cat $DF

grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$dev3" out |tee out3
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3

grep "$PVID1" "$DF" |tee out
grep "$WWID1" out
grep "DEVNAME=$dev1" out

grep "$PVID3" "$DF" |tee out
not grep "$WWID1" out
grep "IDNAME=$dev3" out
grep "DEVNAME=$dev3" out


vgchange -an $vg1
lvremove -y $vg1

