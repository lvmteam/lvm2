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

. lib/inittest --skip-with-lvmpolld

aux lvmconf 'devices/scan = "/dev"'

aux prepare_devs 1

# The tests run with system dir of "/etc" but lvm when running
# normally has cmd->system_dir set to "/etc/lvm".
DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"

# requires trailing / to match dm
SYS_DIR="$PWD/test/sys"
aux lvmconf "devices/use_devicesfile = 1" \
	"devices/device_id_sysfs_dir = \"$SYS_DIR/\"" \
	"devices/device_ids_refresh = 10"

losetup -h | grep sector-size || skip
which fallocate || skip

FILE1="lvmloopfile1"
FILE2="lvmloopfile2"
FILE3="lvmloopfile3"
FILE4="lvmloopfile4"

create_loops() {
	fallocate -l 2M "$FILE1"
	fallocate -l 2M "$FILE2"
	fallocate -l 2M "$FILE3"
	fallocate -l 2M "$FILE4"

	for i in {1..5} ; do
		LOOP1=$(losetup -f "$FILE1" --show || true)
		test -n "$LOOP1" && break
	done
	for i in {1..5} ; do
		LOOP2=$(losetup -f "$FILE2" --show || true)
		test -n "$LOOP2" && break
	done
	for i in {1..5} ; do
		LOOP3=$(losetup -f "$FILE3" --show || true)
		test -n "$LOOP3" && break
	done
	for i in {1..5} ; do
		LOOP4=$(losetup -f "$FILE4" --show || true)
		test -n "$LOOP4" && break
	done
}

remove_loops() {
	losetup -D
	rm "$FILE1"
	rm "$FILE2"
	rm "$FILE3"
	rm "$FILE4"
}

create_pvs() {
	# First two loop devs are original PVs.

	pvcreate --devices "$LOOP1" "$LOOP1"
	pvcreate --devices "$LOOP2" "$LOOP2"

	eval "$(pvs --noheading --nameprefixes -o major,minor,uuid --devices "$LOOP1" "$LOOP1")"
	MAJOR1=$LVM2_PV_MAJOR
	MINOR1=$LVM2_PV_MINOR
	OPVID1=$LVM2_PV_UUID
	PVID1=${OPVID1//-/}

	eval "$(pvs --noheading --nameprefixes -o major,minor,uuid --devices "$LOOP2" "$LOOP2")"
	MAJOR2=$LVM2_PV_MAJOR
	MINOR2=$LVM2_PV_MINOR
	OPVID2=$LVM2_PV_UUID
	PVID2=${OPVID2//-/}

	# Second two loop devs are not originally PVs.
	# Used to move original PVs to new devices.
	# (pvcreate is temporarily used so pvs can report MAJOR/MINOR)

	pvcreate --devices "$LOOP3" "$LOOP3"
	pvcreate --devices "$LOOP4" "$LOOP4"

	eval "$(pvs --noheading --nameprefixes -o major,minor,uuid --devices "$LOOP3" "$LOOP3")"
	MAJOR3=$LVM2_PV_MAJOR
	MINOR3=$LVM2_PV_MINOR

	eval "$(pvs --noheading --nameprefixes -o major,minor,uuid --devices "$LOOP4" "$LOOP4")"
	MAJOR4=$LVM2_PV_MAJOR
	MINOR4=$LVM2_PV_MINOR

	pvremove --devices "$LOOP3" "$LOOP3"
	pvremove --devices "$LOOP4" "$LOOP4"

	dd if="$LOOP1" of=pvheader1 bs=1M count=1
	dd if="$LOOP2" of=pvheader2 bs=1M count=1
}

remove_pvs() {
	rm pvheader1
	rm pvheader2
}

PRODUCT_UUID1="11111111-2222-3333-4444-555555555555"
PRODUCT_UUID2="11111111-2222-3333-4444-666666666666"

create_sysfs() {
	mkdir -p "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/loop"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/loop"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/loop"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/loop"
	mkdir -p "$SYS_DIR/devices/virtual/dmi/id/"
}

remove_sysfs() {
	rm -rf "$SYS_DIR"
}

cleanup_and_teardown()
{
	remove_sysfs
	remove_loops
	remove_pvs
	aux teardown
}

use_12() {
	losetup -D
	losetup "$LOOP1" "$FILE1"
	losetup "$LOOP2" "$FILE2"
	losetup "$LOOP3" "$FILE3"
	losetup "$LOOP4" "$FILE4"
	dd if=pvheader1 of="$LOOP1" bs=1M count=1
	dd if=pvheader2 of="$LOOP2" bs=1M count=1
	aux wipefs_a "$LOOP3"
	aux wipefs_a "$LOOP4"
	losetup -d "$LOOP3"
	losetup -d "$LOOP4"
	rm "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/loop/backing_file"
	rm "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/loop/backing_file"
	echo "$FILE1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/loop/backing_file"
	echo "$FILE2" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/loop/backing_file"
}

use_34() {
	losetup -D
	losetup "$LOOP1" "$FILE1"
	losetup "$LOOP2" "$FILE2"
	losetup "$LOOP3" "$FILE3"
	losetup "$LOOP4" "$FILE4"
	dd if=pvheader1 of="$LOOP3" bs=1M count=1
	dd if=pvheader2 of="$LOOP4" bs=1M count=1
	aux wipefs_a "$LOOP1"
	aux wipefs_a "$LOOP2"
	losetup -d "$LOOP1"
	losetup -d "$LOOP2"
	rm "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/loop/backing_file"
	rm "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/loop/backing_file"
	echo "$FILE3" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/loop/backing_file"
	echo "$FILE4" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/loop/backing_file"
}

detach_1() {
	losetup -d "$LOOP1"
	rm "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/loop/backing_file"
}

attach_1() {
	losetup "$LOOP1" "$FILE1"
	echo "$FILE1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/loop/backing_file"
}

detach_2() {
	losetup -d "$LOOP2"
	rm "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/loop/backing_file"
}

attach_2() {
	losetup "$LOOP2" "$FILE2"
	echo "$FILE2" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/loop/backing_file"
}

detach_3() {
	losetup -d "$LOOP3"
	rm "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/loop/backing_file"
}

attach_3() {
	losetup "$LOOP3" "$FILE3"
	echo "$FILE3" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/loop/backing_file"
}

detach_4() {
	losetup -d "$LOOP4"
	rm "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/loop/backing_file"
}

attach_4() {
	losetup "$LOOP4" "$FILE4"
	echo "$FILE4" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/loop/backing_file"
}

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"
ORIG="$DFDIR/orig.devices"

trap 'cleanup_and_teardown' EXIT

remove_sysfs
create_loops
create_pvs
create_sysfs
losetup -D

########################################################################
# initial state:
# system is UUID1
# PV1 on LOOP1
# PV2 on LOOP2
# no LOOP3 or LOOP4 exist

echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
echo "$FILE1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/loop/backing_file"
echo "$FILE2" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/loop/backing_file"
losetup "$LOOP1" "$FILE1"
losetup "$LOOP2" "$FILE2"

touch "$DF"
lvmdevices --adddev "$LOOP1"
lvmdevices --adddev "$LOOP2"

cat "$DF"
grep "$LOOP1" "$DF"
grep "$LOOP2" "$DF"
not grep "$LOOP3" "$DF"
not grep "$LOOP4" "$DF"
grep "$FILE1" "$DF"
grep "$FILE2" "$DF"
not grep "$FILE3" "$DF"
not grep "$FILE4" "$DF"


########################################################################
# system changes from UUID1 to UUID2
# on new system:
# PV1 is on LOOP3
# PV2 is on LOOP4
# no LOOP1 or LOOP2 exist

echo "$PRODUCT_UUID2" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
use_34

# initial refresh trigger finds both, no REFRESH_UNTIL needed
pvs -o+uuid | tee out
cat "$DF"
grep "$OPVID1" out
grep "$OPVID2" out
grep "$LOOP3" out
grep "$LOOP4" out
grep "$PVID1" "$DF"
grep "$PVID2" "$DF"
grep "$LOOP3" "$DF"
grep "$LOOP4" "$DF"
grep "$FILE3" "$DF"
grep "$FILE4" "$DF"
grep "$PRODUCT_UUID2" "$DF"
not grep REFRESH_UNTIL "$DF"

########################################################################
# system changes from UUID2 to UUID1
# on new system:
# PV1 is on LOOP1
# PV2 is on LOOP2
# no LOOP3 or LOOP4 exist
# PV2 appears after delay

echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
use_12

# will be attached after a delay
detach_2

# initial refresh trigger finds only PV1
pvs -o+uuid | tee out
cat "$DF"
grep "$OPVID1" out
grep "$LOOP1" out
not grep "$OPVID2" out
not grep "$LOOP2" out

# new device ID is shown for PV1
grep "$PVID1" "$DF" | tee line1
grep "$LOOP1" line1
grep "$FILE1" line1

# old device ID still shown for PV2
grep "$PVID2" "$DF" | tee line2
grep "$LOOP4" line2
grep "$FILE4" line2

grep "$PRODUCT_UUID1" "$DF"
grep REFRESH_UNTIL "$DF"

# attach after a delay less than refresh period
sleep 1
attach_2

# second refresh finds both
pvs -o+uuid | tee out
cat "$DF"
grep "$OPVID1" out
grep "$LOOP1" out
grep "$OPVID2" out
grep "$LOOP2" out

# unchanged device ID for PV1
grep "$PVID1" "$DF" | tee line1
grep "$LOOP1" line1
grep "$FILE1" line1

# new devie ID now shown for PV2
grep "$PVID2" "$DF" | tee line2
grep "$LOOP2" line2
grep "$FILE2" line2

grep "$PRODUCT_UUID1" "$DF"
not grep REFRESH_UNTIL "$DF"

########################################################################
# system changes from UUID1 to UUID2
# on new system:
# PV1 is on LOOP3
# PV2 is on LOOP4
# no LOOP1 or LOOP2 exist
# PV1 and PV2 appear after delay

echo "$PRODUCT_UUID2" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
use_34

# will be attached after a delay
detach_3
detach_4

# initial refresh trigger finds none
pvs -o+uuid | tee out
cat "$DF"
not grep "$OPVID1" out
not grep "$LOOP3" out
not grep "$OPVID2" out
not grep "$LOOP4" out

# old device ID still shown for PV1
grep "$PVID1" "$DF" | tee line1
grep "$LOOP1" line1
grep "$FILE1" line1

# old device ID still shown for PV2
grep "$PVID2" "$DF" | tee line2
grep "$LOOP2" line2
grep "$FILE2" line2

grep "$PRODUCT_UUID2" "$DF"
grep REFRESH_UNTIL "$DF"

# attach after a delay less than refresh period
sleep 1
attach_3
attach_4

# second refresh finds both
pvs -o+uuid | tee out
cat "$DF"
grep "$OPVID1" out
grep "$LOOP3" out
grep "$OPVID2" out
grep "$LOOP4" out

# new devie ID now shown for PV1
grep "$PVID1" "$DF" | tee line1
grep "$LOOP3" line1
grep "$FILE3" line1

# new devie ID now shown for PV2
grep "$PVID2" "$DF" | tee line2
grep "$LOOP4" line2
grep "$FILE4" line2

not grep REFRESH_UNTIL "$DF"

########################################################################
# system changes from UUID2 to UUID1
# on new system:
# PV1 is on LOOP1
# PV2 is on LOOP2
# no LOOP3 or LOOP4 exist
# PV1 appears after delay < 10s
# PV2 appears after delay > 10s, not seen by 'pvs' until forced refresh

echo "$PRODUCT_UUID1" > "$SYS_DIR/devices/virtual/dmi/id/product_uuid"
use_12

# will be attached after a delay
detach_1
detach_2

# initial refresh trigger finds none
pvs -o+uuid | tee out
cat "$DF"
not grep "$OPVID1" out
not grep "$LOOP3" out
not grep "$OPVID2" out
not grep "$LOOP4" out

# old device ID still shown for PV1
grep "$PVID1" "$DF" | tee line1
grep "$LOOP3" line1
grep "$FILE3" line1

# old device ID still shown for PV2
grep "$PVID2" "$DF" | tee line2
grep "$LOOP4" line2
grep "$FILE4" line2

grep "$PRODUCT_UUID1" "$DF"
grep REFRESH_UNTIL "$DF"

# attach 1 after a delay less than refresh period
sleep 1
attach_1

# second refresh finds PV1
pvs -o+uuid | tee out
cat "$DF"
grep "$OPVID1" out
grep "$LOOP1" out
not grep "$OPVID2" out
not grep "$LOOP2" out

# new devie ID now shown for PV1
grep "$PVID1" "$DF" | tee line1
grep "$LOOP1" line1
grep "$FILE1" line1

# old devie ID still shown for PV2
grep "$PVID2" "$DF" | tee line2
grep "$LOOP4" line2
grep "$FILE4" line2

grep REFRESH_UNTIL "$DF"

# attach PV2 after a delay larger than refresh period
sleep 10
attach_2

# refresh period expired, so pvs will not refresh and still
# doesn't see 2
pvs -o+uuid | tee out
cat "$DF"
grep "$OPVID1" out
grep "$LOOP1" out
not grep "$OPVID2" out
not grep "$LOOP2" out

# old devie ID still shown for PV2
grep "$PVID2" "$DF" | tee line2
grep "$LOOP4" line2
grep "$FILE4" line2

# refresh expired, so removed from DF
not grep REFRESH_UNTIL "$DF"

# forced refresh now finds 2
lvmdevices --update --refresh

pvs -o+uuid | tee out
cat "$DF"
grep "$OPVID1" out
grep "$LOOP1" out
grep "$OPVID2" out
grep "$LOOP2" out

# new devie ID now shown for PV2
grep "$PVID2" "$DF" | tee line2
grep "$LOOP2" line2
grep "$FILE2" line2

