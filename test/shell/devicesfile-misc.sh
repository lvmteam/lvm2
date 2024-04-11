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

# requires trailing / to match dm
SYS_DIR="$PWD/test/sys"
aux lvmconf "devices/use_devicesfile = 1" \
	"devices/device_id_sysfs_dir = \"$SYS_DIR/\"" \
	'devices/global_filter = [ "a|.*|" ]' \
	'devices/filter = [ "a|.*|" ]' \
	"global/event_activation = 1"


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

#
# lvmdevices --check|--update
#

DEVNAME_NONE=/dev/sdxyz
PVID_NONE=aaaaaa
WWID_NONE=naa.56789

rm $DF
lvmdevices --adddev "$dev1"
lvmdevices --adddev "$dev2"
lvmdevices --adddev "$dev3"
lvmdevices --adddev "$dev4"
cat $DF
cp "$DF" orig

# 1. lvmdevices --check|--update : devs with wwid
# 1.a change pvid and devname
sed -e "s|DEVNAME=$dev1|DEVNAME=$DEVNAME_NONE|" orig > tmp1
sed -e "s|PVID=$PVID1|PVID=$PVID_NONE|" tmp1 > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID1 out
grep DEVNAME=$dev1 out
grep "old $DEVNAME_NONE" out
grep "old $PVID_NONE" out
lvmdevices --update
grep PVID=$PVID1 "$DF" |tee out
grep DEVNAME=$dev1 out
lvmdevices --check

# 1.b change devname
sed -e "s|DEVNAME=$dev1|DEVNAME=$DEVNAME_NONE|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID1 out
grep DEVNAME=$dev1 out
grep "old $DEVNAME_NONE" out
lvmdevices --update
grep PVID=$PVID1 "$DF" |tee out
grep DEVNAME=$dev1 out
lvmdevices --check

# 1.c change pvid
sed -e "s|PVID=$PVID1|PVID=$PVID_NONE|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID1 out
grep DEVNAME=$dev1 out
grep "old $PVID_NONE" out
lvmdevices --update
grep PVID=$PVID1 "$DF" |tee out
grep DEVNAME=$dev1 out
lvmdevices --check

# 2. lvmdevices --check|--update : devs with only devname
# 2.a change idname and devname
sed -e "s|IDNAME=$dev3|IDNAME=$DEVNAME_NONE|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=$DEVNAME_NONE|" tmp1 > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID3 out |tee out1
grep IDNAME=$dev3 out1
grep DEVNAME=$dev3 out1
grep "old $DEVNAME_NONE" out1
lvmdevices --update
grep PVID=$PVID3 "$DF" |tee out
grep IDNAME=$dev3 out
grep DEVNAME=$dev3 out
lvmdevices --check

# 2.b change devname
sed -e "s|DEVNAME=$dev3|DEVNAME=$DEVNAME_NONE|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID3 out |tee out1
grep IDNAME=$dev3 out1
grep DEVNAME=$dev3 out1
grep "old $DEVNAME_NONE" out1
lvmdevices --update
grep PVID=$PVID3 "$DF" |tee out
grep IDNAME=$dev3 out
grep DEVNAME=$dev3 out
lvmdevices --check

# 2.c change idname
sed -e "s|IDNAME=$dev3|IDNAME=$DEVNAME_NONE|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID3 out |tee out1
grep IDNAME=$dev3 out1
grep DEVNAME=$dev3 out1
grep "old $DEVNAME_NONE" out1
lvmdevices --update
grep PVID=$PVID3 "$DF" |tee out
grep IDNAME=$dev3 out
grep DEVNAME=$dev3 out
lvmdevices --check

# 3. lvmdevices --check|--update --refresh devs with IDTYPE=sys_wwid
# 3.a.i change idtype and idname and devname (wwid to serial)
sed -e "s|DEVNAME=$dev1|DEVNAME=$DEVNAME_NONE|" orig > tmp1
sed -e "s|IDTYPE=sys_wwid IDNAME=$WWID1|IDTYPE=sys_serial IDNAME=S123|" tmp1 > "$DF"
cat "$DF"
# this command succeeds since no update is detected, the dev is simply not found
lvmdevices --check |tee out
grep 'device not found' out
not lvmdevices --check --refresh |tee out
grep PVID=$PVID1 out |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
grep "old sys_serial" out1
grep "old S123" out1
lvmdevices --update --refresh
grep PVID=$PVID1 "$DF" |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
lvmdevices --check

# 3.a.ii change idtype and idname and devname (wwid to devname)
sed -e "s|DEVNAME=$dev1|DEVNAME=$DEVNAME_NONE|" orig > tmp1
sed -e "s|IDTYPE=sys_wwid IDNAME=$WWID1|IDTYPE=devname IDNAME=$DEVNAME_NONE|" tmp1 > "$DF"
cat "$DF"
# the command without --refresh thinks the update should be just to the devname
not lvmdevices --check |tee out
grep PVID=$PVID1 out | tee out1
grep DEVNAME=$dev1 out1
grep IDNAME=$dev1 out1
grep IDTYPE=devname out1
# the command with --refresh sees the update should use sys_wwid
not lvmdevices --check --refresh |tee out
grep PVID=$PVID1 out | tee out1
grep DEVNAME=$dev1 out1
grep IDNAME=$WWID1 out1
grep IDTYPE=sys_wwid out1
lvmdevices --update --refresh
grep PVID=$PVID1 "$DF" |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
lvmdevices --check

# 3.b change idtype and idname
sed -e "s|IDTYPE=sys_wwid IDNAME=$WWID1|IDTYPE=sys_serial IDNAME=S123|" orig > "$DF"
cat "$DF"
# this command succeeds since no update is detected, the dev is simply not found
lvmdevices --check |tee out
grep 'device not found' out
not lvmdevices --check --refresh |tee out
grep PVID=$PVID1 out |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
grep "old sys_serial" out1
grep "old S123" out1
lvmdevices --update --refresh
grep PVID=$PVID1 "$DF" |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
lvmdevices --check

# 3.c change idname and devname
sed -e "s|DEVNAME=$dev1|DEVNAME=$DEVNAME_NONE|" orig > tmp1
sed -e "s|IDNAME=$WWID1|IDNAME=$WWID_NONE|" tmp1 > "$DF"
cat "$DF"
# this command succeeds since no update is detected, the dev is simply not found
lvmdevices --check |tee out
grep 'device not found' out
not lvmdevices --check --refresh |tee out
grep PVID=$PVID1 out |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
grep "old $WWID_NONE" out1
lvmdevices --update --refresh
grep PVID=$PVID1 "$DF" |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
lvmdevices --check

# 3.d change idname
sed -e "s|IDNAME=$WWID1|IDNAME=$WWID_NONE|" orig > "$DF"
cat "$DF"
# this command succeeds since no update is detected, the dev is simply not found
lvmdevices --check |tee out
grep 'device not found' out
not lvmdevices --check --refresh |tee out
grep PVID=$PVID1 out |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
grep "old $WWID_NONE" out1
lvmdevices --update --refresh
grep PVID=$PVID1 "$DF" |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
lvmdevices --check

# 3.e change devname
sed -e "s|DEVNAME=$dev1|DEVNAME=$DEVNAME_NONE|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID1 out |tee out1
grep DEVNAME=$dev1 out1
grep "old $DEVNAME_NONE" out1
not lvmdevices --check --refresh |tee out
grep PVID=$PVID1 out |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
lvmdevices --update --refresh
grep PVID=$PVID1 "$DF" |tee out1
grep DEVNAME=$dev1 out1
grep IDTYPE=sys_wwid out1
grep IDNAME=$WWID1 out1
lvmdevices --check

#
# lvmdevices --check|--update with empty fields
#

# PV with wwid, set DEVNAME=. PVID=.
sed -e "s|DEVNAME=$dev1|DEVNAME=.|" orig > tmp1
sed -e "s|PVID=$PVID1|PVID=.|" tmp1 > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID1 out
grep DEVNAME=$dev1 out
grep "old none" out
lvmdevices --update
grep PVID=$PVID1 "$DF" |tee out
grep DEVNAME=$dev1 out
lvmdevices --check

# PV with wwid, set DEVNAME=.
sed -e "s|DEVNAME=$dev1|DEVNAME=.|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID1 out
grep DEVNAME=$dev1 out
grep "old none" out
lvmdevices --update
grep PVID=$PVID1 "$DF" |tee out
grep DEVNAME=$dev1 out
lvmdevices --check

# PV with wwid, set PVID=.
sed -e "s|PVID=$PVID1|PVID=.|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID1 out
grep DEVNAME=$dev1 out
grep "old none" out
lvmdevices --update
grep PVID=$PVID1 "$DF" |tee out
grep DEVNAME=$dev1 out
lvmdevices --check

# PV with wwid, set IDNAME=. DEVNAME=.
sed -e "s|IDNAME=$WWID1|IDNAME=.|" orig > tmp1
sed -e "s|DEVNAME=$dev1|DEVNAME=.|" tmp1 > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep PVID=$PVID1 out |tee out1
grep 'device not found' out1
not lvmdevices --check --refresh |tee out
grep PVID=$PVID1 out |tee out1
grep IDNAME=$WWID1 out1
grep DEVNAME=$dev1 out1
grep "old none" out1
lvmdevices --update --refresh
grep PVID=$PVID1 "$DF" |tee out
grep IDNAME=$WWID1 out
grep DEVNAME=$dev1 out
lvmdevices --check

# PV with wwid, set IDNAME=.
sed -e "s|IDNAME=$WWID1|IDNAME=.|" orig > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep PVID=$PVID1 out |tee out1
grep 'device not found' out1
not lvmdevices --check --refresh |tee out
grep PVID=$PVID1 out |tee out1
grep IDNAME=$WWID1 out1
grep DEVNAME=$dev1 out1
grep "old none" out1
lvmdevices --update --refresh
grep PVID=$PVID1 "$DF" |tee out
grep IDNAME=$WWID1 out
grep DEVNAME=$dev1 out
lvmdevices --check

# PV without wwid, set IDNAME=. DEVNAME=.
sed -e "s|IDNAME=$dev3|IDNAME=.|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=.|" tmp1 > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID3 out |tee out1
grep IDNAME=$dev3 out1
grep DEVNAME=$dev3 out1
grep "old none" out1
lvmdevices --update
grep PVID=$PVID3 "$DF" |tee out
grep IDNAME=$dev3 out
grep DEVNAME=$dev3 out
lvmdevices --check

# PV without wwid, set IDNAME=.
sed -e "s|DEVNAME=$dev3|DEVNAME=.|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID3 out |tee out1
grep IDNAME=$dev3 out1
grep DEVNAME=$dev3 out1
grep "old none" out1
lvmdevices --update
grep PVID=$PVID3 "$DF" |tee out
grep IDNAME=$dev3 out
grep DEVNAME=$dev3 out
lvmdevices --check

# PV without wwid, set DEVNAME=.
sed -e "s|DEVNAME=$dev3|DEVNAME=.|" orig > "$DF"
cat "$DF"
not lvmdevices --check |tee out
grep PVID=$PVID3 out |tee out1
grep IDNAME=$dev3 out1
grep DEVNAME=$dev3 out1
grep "old none" out1
lvmdevices --update
grep PVID=$PVID3 "$DF" |tee out
grep IDNAME=$dev3 out
grep DEVNAME=$dev3 out
lvmdevices --check

# Without a wwid or a pvid, an entry is indeterminate; there's not enough
# info to definitively know what the entry refers to.  These entries will
# never be useful and should be removed.  It could be argued that a
# devname entry with a valid device name set in IDNAME and/or DEVNAME
# should be updated with whatever PVID happens to be found on that device.
# By that logic, any PV that's found on that device would be used by lvm,
# and that violates a central rule of the devices file: that lvm will not
# use a PV unless it's definitively identified in the devices file.  For
# example, consider a PV and a clone of that PV on a different device.
# The devices file should guarantee that the correct PV and not the clone
# will be used.  The user needs to intervene to select one if lvm cannot
# tell the difference.

# PV without wwid, set PVID=.
sed -e "s|PVID=$PVID3|PVID=.|" orig > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep indeterminate out

# PV without wwid, set PVID=. DEVNAME=.
sed -e "s|PVID=$PVID3|PVID=.|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=.|" tmp1 > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep indeterminate out

# PV without wwid, set PVID=. IDNAME=.
sed -e "s|PVID=$PVID3|PVID=.|" orig > tmp1
sed -e "s|IDNAME=$dev3|IDNAME=.|" tmp1 > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep indeterminate out

# PV without wwid, set PVID=. IDNAME=. DEVNAME=.
sed -e "s|PVID=$PVID3|PVID=.|" orig > tmp1
sed -e "s|IDNAME=$dev3|IDNAME=.|" tmp1 > tmp2
sed -e "s|DEVNAME=$dev3|DEVNAME=.|" tmp2 > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep indeterminate out

# indeterminate cases with additional change

# PV without wwid, set PVID=.
sed -e "s|PVID=$PVID3|PVID=.|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=$DEVNAME_NONE|" tmp1 > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep indeterminate out

# PV without wwid, set PVID=. DEVNAME=.
sed -e "s|PVID=$PVID3|PVID=.|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=.|" tmp1 > tmp2
sed -e "s|IDNAME=$dev3|IDNAME=$DEVNAME_NONE|" tmp2 > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep indeterminate out

# PV without wwid, set PVID=. IDNAME=.
sed -e "s|PVID=$PVID3|PVID=.|" orig > tmp1
sed -e "s|IDNAME=$dev3|IDNAME=.|" tmp1 > tmp2
sed -e "s|DEVNAME=$dev3|DEVNAME=$DEVNAME_NONE|" tmp2 > "$DF"
cat "$DF"
lvmdevices --check |tee out
grep indeterminate out

cp orig "$DF"
vgchange -an $vg1
lvremove -y $vg1

