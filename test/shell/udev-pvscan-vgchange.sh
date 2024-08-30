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

test_description='udev rule and systemd unit run vgchange'

SKIP_WITH_LVMPOLLD=1
SKIP_WITH_LVMLOCKD=1

. lib/inittest

# FIXME: currently test relies on several system properties to be
# explicitly configure and directly modifies their state

#
# $ cat /tmp/devs
# /dev/sdb
# /dev/sdc
# /dev/sdd
#
# Specify this file as LVM_TEST_DEVICE_LIST=/tmp/devs
# when running the test.
#
# This test will wipe these devices.
#

if [ -z ${LVM_TEST_DEVICE_LIST+x} ]; then
	skip "LVM_TEST_DEVICE_LIST is unset"
else
	echo "LVM_TEST_DEVICE_LIST is set to '$LVM_TEST_DEVICE_LIST'"
fi

test -e "$LVM_TEST_DEVICE_LIST" || skip

num_devs=$(wc -l < "$LVM_TEST_DEVICE_LIST")

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

test -d "$PVS_ONLINE_DIR" || mkdir -p "$PVS_ONLINE_DIR"
test -d "$VGS_ONLINE_DIR" || mkdir -p "$VGS_ONLINE_DIR"
test -d "$PVS_LOOKUP_DIR" || mkdir -p "$PVS_LOOKUP_DIR"
_clear_online_files

aux prepare_real_devs

aux lvmconf 'devices/dir = "/dev"' \
	    'devices/use_devicesfile = 1'
DFDIR="$LVM_SYSTEM_DIR/devices"
DF="$DFDIR/system.devices"
mkdir "$DFDIR" || true
not ls "$DF"

get_real_devs

wipe_all() {
	for dev in "${REAL_DEVICES[@]}"; do
		wipefs -a "$dev"
	done
}

wait_lvm_activate() {
	local vgw=$1
	local wait=0
	rm status || true

	# time for service to be started
	sleep 1

 	while systemctl status lvm-activate-$vgw |tee status && test "$wait" -le 30; do
 		sleep .2
 		wait=$(( wait + 1 ))
	done
	cat status || true
}

# Test requires 3 devs
test "$num_devs" -gt 2 || skip
BDEV1=$(basename "$dev1")
BDEV2=$(basename "$dev2")
BDEV3=$(basename "$dev3")

wipe_all
touch "$DF"
for dev in "${REAL_DEVICES[@]}"; do
	pvcreate $dev
done

# 1 dev, 1 vg, 1 lv

vgcreate $vg1 "$dev1"
lvcreate -l1 -an -n $lv1 $vg1 "$dev1"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')

_clear_online_files
udevadm trigger --settle -c add "/sys/block/$BDEV1"

wait_lvm_activate $vg1

ls "$RUNDIR/lvm/pvs_online/$PVID1" || true
ls "$RUNDIR/lvm/vgs_online/$vg1" || true
journalctl -u lvm-activate-$vg1 | tee out || true
grep "now active" out
check lv_field $vg1/$lv1 lv_active "active"

vgchange -an $vg1
vgremove -y $vg1


# 2 devs, 1 vg, 2 lvs

vgcreate $vg2 "$dev1" "$dev2"
lvcreate -l1 -an -n $lv1 $vg2 "$dev1"
lvcreate -l1 -an -n $lv2 $vg2 "$dev2"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID2=$(pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}')

_clear_online_files

udevadm trigger --settle -c add "/sys/block/$BDEV1"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
not ls "$RUNDIR/lvm/vgs_online/$vg2"
journalctl -u lvm-activate-$vg2 | tee out || true
not grep "now active" out
check lv_field $vg2/$lv1 lv_active ""
check lv_field $vg2/$lv2 lv_active ""

udevadm trigger --settle -c add "/sys/block/$BDEV2"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/vgs_online/$vg2"

wait_lvm_activate $vg2

journalctl -u lvm-activate-$vg2 | tee out || true
grep "now active" out
check lv_field $vg2/$lv1 lv_active "active"
check lv_field $vg2/$lv2 lv_active "active"

vgchange -an $vg2
vgremove -y $vg2


# 3 devs, 1 vg, 4 lvs, concurrent pvscans
# (attempting to have the pvscans run concurrently and race
# to activate the VG)

vgcreate $vg3 "$dev1" "$dev2" "$dev3"
lvcreate -l1 -an -n $lv1 $vg3 "$dev1"
lvcreate -l1 -an -n $lv2 $vg3 "$dev2"
lvcreate -l1 -an -n $lv3 $vg3 "$dev3"
lvcreate -l8 -an -n $lv4 -i 2 $vg3 "$dev1" "$dev2"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID2=$(pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID3=$(pvs "$dev3" --noheading -o uuid | tr -d - | awk '{print $1}')

_clear_online_files

udevadm trigger -c add "/sys/block/$BDEV1" &
udevadm trigger -c add "/sys/block/$BDEV2" &
udevadm trigger -c add "/sys/block/$BDEV3"

aux udev_wait
wait_lvm_activate $vg3

find "$RUNDIR/lvm"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
ls "$RUNDIR/lvm/vgs_online/$vg3"

journalctl -u lvm-activate-$vg3 | tee out || true
grep "now active" out
check lv_field $vg3/$lv1 lv_active "active"
check lv_field $vg3/$lv2 lv_active "active"
check lv_field $vg3/$lv3 lv_active "active"
check lv_field $vg3/$lv4 lv_active "active"

vgchange -an $vg3
vgremove -y $vg3


# 3 devs, 1 vg, 4 lvs, concurrent pvscans, metadata on only 1 PV

wipe_all
rm $DF
touch $DF
pvcreate --metadatacopies 0 "$dev1"
pvcreate --metadatacopies 0 "$dev2"
pvcreate "$dev3"

vgcreate $vg4 "$dev1" "$dev2" "$dev3"
lvcreate -l1 -an -n $lv1 $vg4 "$dev1"
lvcreate -l1 -an -n $lv2 $vg4 "$dev2"
lvcreate -l1 -an -n $lv3 $vg4 "$dev3"
lvcreate -l8 -an -n $lv4 -i 2 $vg4 "$dev1" "$dev2"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID2=$(pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID3=$(pvs "$dev3" --noheading -o uuid | tr -d - | awk '{print $1}')

_clear_online_files

udevadm trigger -c add "/sys/block/$BDEV1" &
udevadm trigger -c add "/sys/block/$BDEV2" &
udevadm trigger -c add "/sys/block/$BDEV3"

aux udev_wait
wait_lvm_activate $vg4

ls "$RUNDIR/lvm/pvs_lookup/"
cat "$RUNDIR/lvm/pvs_lookup/$vg4" || true
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
ls "$RUNDIR/lvm/vgs_online/$vg4"
journalctl -u lvm-activate-$vg4 | tee out || true
grep "now active" out
check lv_field $vg4/$lv1 lv_active "active"
check lv_field $vg4/$lv2 lv_active "active"
check lv_field $vg4/$lv3 lv_active "active"
check lv_field $vg4/$lv4 lv_active "active"

vgchange -an $vg4
vgremove -y $vg4


# 3 devs, 3 vgs, 2 lvs in each vg, concurrent pvscans

wipe_all
rm "$DF"
touch "$DF"

vgcreate $vg5 "$dev1"
vgcreate $vg6 "$dev2"
vgcreate $vg7 "$dev3"
lvcreate -l1 -an -n $lv1 $vg5
lvcreate -l1 -an -n $lv2 $vg5
lvcreate -l1 -an -n $lv1 $vg6
lvcreate -l1 -an -n $lv2 $vg6
lvcreate -l1 -an -n $lv1 $vg7
lvcreate -l1 -an -n $lv2 $vg7

_clear_online_files

udevadm trigger -c add "/sys/block/$BDEV1" &
udevadm trigger -c add "/sys/block/$BDEV2" &
udevadm trigger -c add "/sys/block/$BDEV3"

aux udev_wait
wait_lvm_activate $vg5
wait_lvm_activate $vg6
wait_lvm_activate $vg7

ls "$RUNDIR/lvm/vgs_online/$vg5"
ls "$RUNDIR/lvm/vgs_online/$vg6"
ls "$RUNDIR/lvm/vgs_online/$vg7"
journalctl -u lvm-activate-$vg5 | tee out || true
grep "now active" out
journalctl -u lvm-activate-$vg6 | tee out || true
grep "now active" out
journalctl -u lvm-activate-$vg7 | tee out || true
grep "now active" out
check lv_field $vg5/$lv1 lv_active "active"
check lv_field $vg5/$lv2 lv_active "active"
check lv_field $vg6/$lv1 lv_active "active"
check lv_field $vg6/$lv2 lv_active "active"
check lv_field $vg7/$lv1 lv_active "active"
check lv_field $vg7/$lv2 lv_active "active"

vgchange -an $vg5
vgremove -y $vg5
vgchange -an $vg6
vgremove -y $vg6
vgchange -an $vg7
vgremove -y $vg7

# 3 devs, 1 vg, 1000 LVs

wipe_all
rm "$DF"
touch "$DF"
pvcreate --metadatacopies 0 "$dev1"
pvcreate "$dev2"
pvcreate "$dev3"
vgcreate -s 128K $vg8 "$dev1" "$dev2" "$dev3"

# Number of LVs to create
TEST_DEVS=1000
# On low-memory boxes let's not stress too much
test "$(aux total_mem)" -gt 524288 || TEST_DEVS=256

vgcfgbackup -f data $vg8

# Generate a lot of devices (size of 1 extent)
awk -v TEST_DEVS=$TEST_DEVS '/^\t\}/ {
    printf("\t}\n\tlogical_volumes {\n");
    cnt=0;
    for (i = 0; i < TEST_DEVS; i++) {
        printf("\t\tlvol%06d  {\n", i);
        printf("\t\t\tid = \"%06d-1111-2222-3333-2222-1111-%06d\"\n", i, i);
        print "\t\t\tstatus = [\"READ\", \"WRITE\", \"VISIBLE\"]";
        print "\t\t\tsegment_count = 1";
        print "\t\t\tsegment1 {";
        print "\t\t\t\tstart_extent = 0";
        print "\t\t\t\textent_count = 1";
        print "\t\t\t\ttype = \"striped\"";
        print "\t\t\t\tstripe_count = 1";
        print "\t\t\t\tstripes = [";
        print "\t\t\t\t\t\"pv0\", " cnt++;
        printf("\t\t\t\t]\n\t\t\t}\n\t\t}\n");
      }
  }
  {print}
' data >data_new

vgcfgrestore -f data_new $vg8

_clear_online_files

udevadm trigger -c add "/sys/block/$BDEV1" &
udevadm trigger -c add "/sys/block/$BDEV2" &
udevadm trigger -c add "/sys/block/$BDEV3"

aux udev_wait
wait_lvm_activate $vg8

ls "$RUNDIR/lvm/vgs_online/$vg8"
journalctl -u lvm-activate-$vg8 | tee out || true
grep "now active" out

num_active=$(lvs $vg8 --noheading -o active | grep -c active)

test "$num_active" -eq "$TEST_DEVS"

vgchange -an $vg8
vgremove -y $vg8

# 1 pv on an md dev, 1 vg

wait_md_create() {
	local md=$1

	while :; do
		if ! grep "$(basename $md)" /proc/mdstat; then
			echo "$md not ready"
			cat /proc/mdstat
			sleep 2
		else
			break
		fi
	done
	echo "$md" > WAIT_MD_DEV
}

test -f /proc/mdstat && grep -q raid1 /proc/mdstat || \
       modprobe raid1 || skip

wipe_all
rm "$DF"
touch "$DF"

aux mdadm_create --metadata=1.0 --level 1 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
mddev=$(< MD_DEV)

wait_md_create "$mddev"
vgcreate $vg9 "$mddev"
lvmdevices --adddev "$mddev" || true

PVIDMD=$(pvs "$mddev" --noheading -o uuid | tr -d - | awk '{print $1}')
BDEVMD=$(basename "$mddev")

lvcreate -l1 -an -n $lv1 $vg9
lvcreate -l1 -an -n $lv2 $vg9

mdadm --stop "$mddev"
_clear_online_files
aux mdadm_assemble "$mddev" "$dev1" "$dev2"

# this trigger might be redundant because the mdadm --assemble
# probably triggers an add uevent
udevadm trigger --settle -c add /sys/block/$BDEVMD

wait_lvm_activate $vg9

ls "$RUNDIR/lvm/vgs_online/$vg9"
journalctl -u lvm-activate-$vg9 | tee out || true
grep "now active" out
check lv_field $vg9/$lv1 lv_active "active"
check lv_field $vg9/$lv2 lv_active "active"

vgchange -an $vg9
vgremove -y $vg9

mdadm --stop "$mddev"
aux udev_wait
wipe_all

# no devices file, filter with symlink of PV
# the pvscan needs to look at all dev names to
# match the symlink in the filter with the
# dev name (or major minor) passed to pvscan.
# This test doesn't really belong in this file
# because it's not testing lvm-activate.

aux lvmconf 'devices/use_devicesfile = 0'
_clear_online_files
rm "$DF"
vgcreate $vg10 "$dev1"
lvcreate -l1 -an -n $lv1 $vg10 "$dev1"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')
# PVID with dashes
OPVID1=$(pvs "$dev1" --noheading -o uuid | awk '{print $1}')

udevadm trigger --settle -c add "/sys/block/$BDEV1"

# uevent from the trigger should create this symlink
ls "/dev/disk/by-id/lvm-pv-uuid-$OPVID1"

vgchange -an $vg10
_clear_online_files

aux lvmconf "devices/filter = [ \"a|/dev/disk/by-id/lvm-pv-uuid-$OPVID1|\", \"r|.*|\" ]" \
	    'devices/global_filter = [ "a|.*|" ]'

pvscan --cache -aay "$dev1"

check lv_field $vg10/$lv1 lv_active "active"

vgchange -an $vg10
_clear_online_files

aux lvmconf 'devices/filter = [ "a|lvm-pv-uuid|", "r|.*|" ]' \
	    'devices/global_filter = [ "a|.*|" ]'

pvscan --cache -aay "$dev1"

check lv_field $vg10/$lv1 lv_active "active"

vgchange -an $vg10
vgremove -y $vg10
wipe_all

aux lvmconf 'devices/filter = [ "a|.*|" ]' \
	    'devices/global_filter = [ "a|.*|" ]'

#
# system.devices contains different product_uuid and incorrect device IDs
#

SYS_DIR="$PWD/test/sys"

aux lvmconf 'devices/use_devicesfile = 1' \
	    'devices/device_id_sysfs_dir = \"$SYS_DIR/\"'

WWID1="naa.111"
WWID2="naa.222"
PRODUCT_UUID1="11111111-2222-3333-4444-555555555555"
PRODUCT_UUID2="11111111-2222-3333-4444-666666666666"

vgcreate $vg11 "$dev1"
lvcreate -l1 -an -n $lv1 $vg11 "$dev1"

eval "$(pvs --noheading --nameprefixes -o major,minor,uuid "$dev1")"
MAJOR1=$LVM2_PV_MAJOR
MINOR1=$LVM2_PV_MINOR
OPVID1=$LVM2_PV_UUID
PVID1=${OPVID1//-/}

mkdir -p "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device"
echo "$WWID1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid"
mkdir -p "$SYS_DIR/devices/virtual/dmi/id/"
echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"

vgimportdevices $vg11

grep $PRODUCT_UUID1 "$DF"
grep $PVID1 "$DF"
grep $WWID1 "$DF"
grep "$dev1" "$DF"

# change wwid for dev1 and product_uuid for host

echo "$WWID2" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/wwid"
echo "$PRODUCT_UUID2" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"

_clear_online_files

udevadm trigger --settle -c add "/sys/block/$BDEV1"

wait_lvm_activate $vg11

ls "$RUNDIR/lvm/vgs_online/$vg11"
journalctl -u lvm-activate-$vg11 | tee out || true
grep "now active" out

# Run ordinary command that will refresh device ID in system.devices
pvs -o+uuid | tee out
grep "$dev1" out
grep "$OPVID1" out

# check new wwid for dev1 and new product_uuid for host
cat "$DF"
grep $PRODUCT_UUID2 "$DF"
not grep $PRODUCT_UUID1 "$DF"
grep $PVID1 "$DF"
grep $WWID2 "$DF"
not grep $WWID1 "$DF"
grep "$dev1" "$DF"

check lv_field $vg11/$lv1 lv_active "active"

vgchange -an $vg11
vgremove -y $vg11

rm -rf "$SYS_DIR"

wipe_all

