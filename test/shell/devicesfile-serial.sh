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

test_description='device id wwid from vpd_pg83'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

test -d /sys/block/ram0 && skip "Ramdisk already loaded"

test "$DM_DEV_DIR" = "/dev" || skip "Only works with /dev access -> make check LVM_TEST_DEVDIR=/dev"

# requires trailing / to match dm
SYS_DIR="$PWD/test/sys"
aux lvmconf "devices/use_devicesfile = 1" \
	"devices/device_id_sysfs_dir = \"$SYS_DIR/\""

# The string format of the serial numbers
# encoded in the pg80 files
SERIAL1="003dd33a331c183c2300e1d883604609"
SERIAL2="003dd33a441c183c2300e1d883604609"
SERIAL3="003dd33a551c183c2300e1d883604609"
SERIAL4="003dd33a661c183c2300e1d883604609"

create_base() {
	mkdir -p "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device"
	mkdir -p "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device"

	# Create four different pg80 serial numbers that
	# can be assigned to devs

	echo -n "0080 0020 3030 3364 6433 3361 3333 3163 \
	3138 3363 3233 3030 6531 6438 3833 3630 3436 3039" | xxd -r -p > pg80_1

	echo -n "0080 0020 3030 3364 6433 3361 3434 3163 \
	3138 3363 3233 3030 6531 6438 3833 3630 3436 3039" | xxd -r -p > pg80_2

	echo -n "0080 0020 3030 3364 6433 3361 3535 3163 \
	3138 3363 3233 3030 6531 6438 3833 3630 3436 3039" | xxd -r -p > pg80_3

	echo -n "0080 0020 3030 3364 6433 3361 3636 3163 \
	3138 3363 3233 3030 6531 6438 3833 3630 3436 3039" | xxd -r -p > pg80_4
}

remove_base() {
	rm -rf "$SYS_DIR"
}

cleanup_and_teardown()
{
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

devs=( "$dev1" "$dev2" "$dev3" "$dev4" )

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"
ORIG="$DFDIR/orig.devices"
touch "$DF"

aux wipefs_a "${devs[@]}"

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


# get serial number from pg80
cp pg80_1 "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/vpd_pg80"
cp pg80_2 "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/vpd_pg80"
cp pg80_3 "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/vpd_pg80"
cp pg80_4 "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/vpd_pg80"

grep -r "" "$SYS_DIR/dev/block/"

declare -A serials=( ["$dev1"]=$SERIAL1 ["$dev2"]=$SERIAL2 \
 ["$dev3"]=$SERIAL3 ["$dev4"]=$SERIAL4 )

rm -f "$DF"

for i in "${devs[@]}"
do
	lvmdevices --adddev "$i"
	grep "${serials["$i"]}" "$DF" || {
		cat "$DF"
		die "Cannot find ${serials["$i"]}"
	}
done
cat "$DF"
cp "$DF" "$ORIG"
pvs
# run command to update metadata so deviceids are written to metadata
vgchange --addtag x $vg1
vgchange --addtag x $vg2
vgchange --addtag x $vg3
vgchange --addtag x $vg4
pvs -o uuid,deviceidtype,deviceid "$dev1" |tee out
grep "$OPVID1" out
grep sys_serial out
grep "$SERIAL1" out
pvs -o uuid,deviceidtype,deviceid "$dev2" |tee out
grep "$OPVID2" out
grep sys_serial out
grep "$SERIAL2" out

# get serial number from device/serial

aux wipefs_a "${devs[@]}"

rm "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/vpd_pg80"
rm "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/vpd_pg80"
rm "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/vpd_pg80"
rm "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/vpd_pg80"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL2" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL3" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "$SERIAL4" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
pvcreate "$dev1"
pvcreate "$dev2"
pvcreate "$dev3"
pvcreate "$dev4"
grep "$SERIAL1" "$DF"
grep "$SERIAL2" "$DF"
grep "$SERIAL3" "$DF"
grep "$SERIAL4" "$DF"

# all pvs have the same serial number

aux wipefs_a "${devs[@]}"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
vgcreate $vg3 "$dev3"
vgcreate $vg4 "$dev4"
cp "$DF" "$ORIG"
OPVID1=$(echo $(pvs --noheading -o uuid "$dev1") )
OPVID2=$(echo $(pvs --noheading -o uuid "$dev2") )
OPVID3=$(echo $(pvs --noheading -o uuid "$dev3") )
OPVID4=$(echo $(pvs --noheading -o uuid "$dev4") )
PVID1=${OPVID1//-/}
PVID2=${OPVID2//-/}
PVID3=${OPVID3//-/}
PVID4=${OPVID4//-/}

grep "$PVID1" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev1" out
grep "$PVID2" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev2" out
grep "$PVID3" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev3" out
grep "$PVID4" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev4" out

pvs -o+uuid,deviceidtype,deviceid "${devs[@]}" |tee out
grep "$dev1" out
grep "$dev2" out
grep "$dev3" out
grep "$dev4" out
grep "$OPVID1" out
grep "$OPVID2" out
grep "$OPVID3" out
grep "$OPVID4" out
grep $vg1 out
grep $vg2 out
grep $vg3 out
grep $vg4 out
grep sys_serial out
grep "$SERIAL1" out

pvs -o+uuid,deviceid "$dev1" |tee out
grep "$OPVID1" out
grep "$SERIAL1" out
grep $vg1 out

pvs -o+uuid,deviceid "$dev2" |tee out
grep "$OPVID2" out
grep "$SERIAL1" out
grep $vg2 out

pvs -o+uuid,deviceid "$dev3" |tee out
grep "$OPVID3" out
grep "$SERIAL1" out
grep $vg3 out

pvs -o+uuid,deviceid "$dev4" |tee out
grep "$OPVID4" out
grep "$SERIAL1" out
grep $vg4 out


# all pvs have the same serial number, df devnames are stale
# edit DF to make devnames stale

cp "$ORIG" orig
sed -e "s|DEVNAME=$dev1|DEVNAME=tmpnm|" orig > tmp1
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1 > tmp2
sed -e "s|DEVNAME=tmpnm|DEVNAME=$dev2|" tmp2 > tmp3
sed -e "s|DEVNAME=$dev3|DEVNAME=tmpnm|" tmp3 > tmp4
sed -e "s|DEVNAME=$dev4|DEVNAME=$dev3|" tmp4 > tmp5
sed -e "s|DEVNAME=tmpnm|DEVNAME=$dev4|" tmp5 > "$DF"
cat "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" |tee out
grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$dev3" out |tee out3
grep "$dev4" out |tee out4
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3
grep "$OPVID4" out4
grep "$SERIAL1" out1
grep "$SERIAL1" out2
grep "$SERIAL1" out3
grep "$SERIAL1" out4

grep "$PVID1" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev1" out
grep "$PVID2" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev2" out
grep "$PVID3" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev3" out
grep "$PVID4" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev4" out

pvs -o+uuid,deviceid "$dev1"|tee out1
pvs -o+uuid,deviceid "$dev2"|tee out2
pvs -o+uuid,deviceid "$dev3"|tee out3
pvs -o+uuid,deviceid "$dev4"|tee out4
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3
grep "$OPVID4" out4

# all pvs have the same serial number,
# dev1 and dev2 have devnames swapped,
# dev3 has stale PVID in the DF.
# lvm fixes the stale devnames but does not fix the stale PVID
# because of the duplicate serial numbers, so dev3 is not found

cp "$ORIG" orig
sed -e "s|DEVNAME=$dev1|DEVNAME=tmpnm|" orig > tmp1
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1 > tmp2
sed -e "s|PVID=$PVID4|PVID=4SqT4onBxSiv4dot0GRDPtrWqOlrOPH1|" tmp2 > "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" > out || true
cat out
not grep "$dev4" out
not grep "$OPVID4" out
grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$dev3" out |tee out3
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3

not pvs "$dev4"

# dev1&2 have same serial, dev3&4 have same serial

aux wipefs_a "${devs[@]}"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL2" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "$SERIAL2" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
vgcreate $vg3 "$dev3"
vgcreate $vg4 "$dev4"
cp "$DF" "$ORIG"
OPVID1=$(echo $(pvs --noheading -o uuid "$dev1") )
OPVID2=$(echo $(pvs --noheading -o uuid "$dev2") )
OPVID3=$(echo $(pvs --noheading -o uuid "$dev3") )
OPVID4=$(echo $(pvs --noheading -o uuid "$dev4") )
PVID1=${OPVID1//-/}
PVID2=${OPVID2//-/}
PVID3=${OPVID3//-/}
PVID4=${OPVID4//-/}

grep "$PVID1" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev1" out
grep "$PVID2" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev2" out
grep "$PVID3" "$DF" |tee out
grep "$SERIAL2" out
grep "$dev3" out
grep "$PVID4" "$DF" |tee out
grep "$SERIAL2" out
grep "$dev4" out

pvs -o+uuid,deviceidtype,deviceid "${devs[@]}" |tee out
grep "$dev1" out
grep "$dev2" out
grep "$dev3" out
grep "$dev4" out
grep "$OPVID1" out
grep "$OPVID2" out
grep "$OPVID3" out
grep "$OPVID4" out
grep $vg1 out
grep $vg2 out
grep $vg3 out
grep $vg4 out
grep sys_serial out
grep "$SERIAL1" out
grep "$SERIAL2" out

pvs -o+uuid,deviceid "$dev1" |tee out
grep "$OPVID1" out
grep "$SERIAL1" out
grep $vg1 out

pvs -o+uuid,deviceid "$dev2" |tee out
grep "$OPVID2" out
grep "$SERIAL1" out
grep $vg2 out

pvs -o+uuid,deviceid "$dev3" |tee out
grep "$OPVID3" out
grep "$SERIAL2" out
grep $vg3 out

pvs -o+uuid,deviceid "$dev4" |tee out
grep "$OPVID4" out
grep "$SERIAL2" out
grep $vg4 out

# dev1&2 have serial1 and dev3&4 have serial2, swap devnames
# edit DF to make devnames stale

cp "$ORIG" orig
sed -e "s|DEVNAME=$dev1|DEVNAME=tmpnm|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=$dev1|" tmp1 > tmp2
sed -e "s|DEVNAME=tmpnm|DEVNAME=$dev3|" tmp2 > tmp3
sed -e "s|DEVNAME=$dev2|DEVNAME=tmpnm|" tmp3 > tmp4
sed -e "s|DEVNAME=$dev4|DEVNAME=$dev2|" tmp4 > tmp5
sed -e "s|DEVNAME=tmpnm|DEVNAME=$dev4|" tmp5 > "$DF"
cat "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" |tee out
grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$dev3" out |tee out3
grep "$dev4" out |tee out4
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3
grep "$OPVID4" out4
grep "$SERIAL1" out1
grep "$SERIAL1" out2
grep "$SERIAL2" out3
grep "$SERIAL2" out4

grep "$PVID1" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev1" out
grep "$PVID2" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev2" out
grep "$PVID3" "$DF" |tee out
grep "$SERIAL2" out
grep "$dev3" out
grep "$PVID4" "$DF" |tee out
grep "$SERIAL2" out
grep "$dev4" out

pvs -o+uuid,deviceid "$dev1"|tee out1
pvs -o+uuid,deviceid "$dev2"|tee out2
pvs -o+uuid,deviceid "$dev3"|tee out3
pvs -o+uuid,deviceid "$dev4"|tee out4
grep "$OPVID1" out1
grep "$SERIAL1" out1
grep "$OPVID2" out2
grep "$SERIAL1" out2
grep "$OPVID3" out3
grep "$SERIAL2" out3
grep "$OPVID4" out4
grep "$SERIAL2" out4


# all devs have same serial, dev1&4 are pvs, dev2&3 are not pvs

aux wipefs_a "${devs[@]}"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg1 "$dev1"
vgcreate $vg4 "$dev4"
cp "$DF" "$ORIG"
OPVID1=$(echo $(pvs --noheading -o uuid "$dev1") )
OPVID4=$(echo $(pvs --noheading -o uuid "$dev4") )
PVID1=${OPVID1//-/}
PVID4=${OPVID4//-/}

grep "$PVID1" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev1" out
grep "$PVID4" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev4" out

pvs -o+uuid,deviceidtype,deviceid |tee out
grep "$dev1" out
grep "$dev4" out
grep "$OPVID1" out
grep "$OPVID4" out
grep $vg1 out
grep $vg4 out
grep sys_serial out
grep "$SERIAL1" out

pvs -o+uuid,deviceid "$dev1" |tee out
grep "$OPVID1" out
grep "$SERIAL1" out
grep $vg1 out

not pvs -o+uuid,deviceid "$dev2"
not pvs -o+uuid,deviceid "$dev3"

pvs -o+uuid,deviceid "$dev4" |tee out
grep "$OPVID4" out
grep "$SERIAL1" out
grep $vg4 out

# edit DF to make devnames stale

cp "$ORIG" orig
sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" orig > tmp1
sed -e "s|DEVNAME=$dev4|DEVNAME=$dev3|" tmp1 > "$DF"
cat "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" > out || true
cat out
grep "$dev1" out |tee out1
grep "$dev4" out |tee out4
grep "$OPVID1" out1
grep "$OPVID4" out4
grep "$SERIAL1" out1
grep "$SERIAL1" out4

grep "$PVID1" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev1" out
grep "$PVID4" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev4" out

pvs -o+uuid,deviceid "$dev1"|tee out1
pvs -o+uuid,deviceid "$dev4"|tee out4
grep "$OPVID1" out1
grep "$SERIAL1" out1
grep "$OPVID4" out4
grep "$SERIAL1" out4

# one pv with serial, three other non-pvs with same serial

aux wipefs_a "${devs[@]}"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg2 "$dev2"
cp "$DF" "$ORIG"
OPVID2=$(echo $(pvs --noheading -o uuid "$dev2") )
PVID2=${OPVID2//-/}

grep "$PVID2" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev2" out

pvs -o+uuid,deviceidtype,deviceid |tee out
grep "$dev2" out
grep sys_serial out
grep "$SERIAL1" out
not grep "$dev1" out
not grep "$dev3" out
not grep "$dev4" out

# edit DF to make devname stale

cp "$ORIG" orig
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev3|" orig > "$DF"
cat "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" > out || true
cat out
grep "$dev2" out
grep "$OPVID2" out
grep "$SERIAL1" out
grep "$dev2" "$DF"

# different serial numbers, stale pvid and devname in df,
# lvm corrects pvid in df because serial number is unique

aux wipefs_a "${devs[@]}"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL2" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL3" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "$SERIAL4" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
vgcreate $vg3 "$dev3"
vgcreate $vg4 "$dev4"
cp "$DF" "$ORIG"
grep "$SERIAL1" "$DF"
grep "$SERIAL2" "$DF"
grep "$SERIAL3" "$DF"
grep "$SERIAL4" "$DF"
OPVID1=$(echo $(pvs --noheading -o uuid "$dev1") )
OPVID2=$(echo $(pvs --noheading -o uuid "$dev2") )
OPVID3=$(echo $(pvs --noheading -o uuid "$dev3") )
OPVID4=$(echo $(pvs --noheading -o uuid "$dev4") )
PVID1=${OPVID1//-/}
PVID2=${OPVID2//-/}
PVID3=${OPVID3//-/}
PVID4=${OPVID4//-/}
pvs -o+uuid,deviceid "${devs[@]}"

cp "$ORIG" orig
sed -e "s|PVID=$PVID1|PVID=bad14onBxSiv4dot0GRDPtrWqOlr1bad|" orig > tmp1
sed -e "s|PVID=$PVID3|PVID=bad24onBxSiv4dot0GRDPtrWqOlr2bad|" tmp1 > tmp2
sed -e "s|DEVNAME=$dev1|DEVNAME=.|" tmp2 > "$DF"
cat "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" |tee out
grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$dev3" out |tee out3
grep "$dev4" out |tee out4
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3
grep "$OPVID4" out4
grep $vg1 out1
grep $vg2 out2
grep $vg3 out3
grep $vg4 out4
grep "$SERIAL1" out1
grep "$SERIAL2" out2
grep "$SERIAL3" out3
grep "$SERIAL4" out4

grep "$PVID1" "$DF" |tee out
grep "$SERIAL1" out
grep "$dev1" out
grep "$PVID2" "$DF" |tee out
grep "$SERIAL2" out
grep "$dev2" out
grep "$PVID3" "$DF" |tee out
grep "$SERIAL3" out
grep "$dev3" out
grep "$PVID4" "$DF" |tee out
grep "$SERIAL4" out
grep "$dev4" out

# duplicate serial on two pvs, two pvs with devname type, all devnames stale

aux wipefs_a "${devs[@]}"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
vgcreate $vg3 "$dev3"
vgcreate $vg4 "$dev4"
cp "$DF" "$ORIG"
OPVID1=$(echo $(pvs --noheading -o uuid "$dev1") )
OPVID2=$(echo $(pvs --noheading -o uuid "$dev2") )
OPVID3=$(echo $(pvs --noheading -o uuid "$dev3") )
OPVID4=$(echo $(pvs --noheading -o uuid "$dev4") )
PVID1=${OPVID1//-/}
PVID2=${OPVID2//-/}
PVID3=${OPVID3//-/}
PVID4=${OPVID4//-/}
cat "$DF"

pvs -o+uuid,deviceid "${devs[@]}"

cp "$ORIG" orig
sed -e "s|DEVNAME=$dev1|DEVNAME=tmpnm|" orig > tmp1
sed -e "s|DEVNAME=$dev3|DEVNAME=$dev1|" tmp1 > tmp2
sed -e "s|DEVNAME=tmpnm|DEVNAME=$dev3|" tmp2 > tmp3
sed -e "s|DEVNAME=$dev2|DEVNAME=tmpnm|" tmp3 > tmp4
sed -e "s|DEVNAME=$dev4|DEVNAME=$dev2|" tmp4 > tmp5
sed -e "s|DEVNAME=tmpnm|DEVNAME=$dev4|" tmp5 > "$DF"
cat "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" |tee out
grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$dev3" out |tee out3
grep "$dev4" out |tee out4
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3
grep "$OPVID4" out4
grep $vg1 out1
grep $vg2 out2
grep $vg3 out3
grep $vg4 out4
grep "$SERIAL1" out1
grep "$SERIAL1" out2

cat "$DF"
grep "$PVID1" "$DF" |tee out1
grep "$PVID2" "$DF" |tee out2
grep "$PVID3" "$DF" |tee out3
grep "$PVID4" "$DF" |tee out4
grep "$dev1" out1
grep "$SERIAL1" out1
grep "$dev2" out2
grep "$SERIAL1" out2
grep "$dev3" out3
grep "$dev4" out4

# two pvs with duplicate serial and stale devname, one pv with unique serial and stale pvid

aux wipefs_a "${devs[@]}"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL3" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg1 "$dev1"
vgcreate $vg2 "$dev2"
vgcreate $vg3 "$dev3"
cp "$DF" "$ORIG"
OPVID1=$(echo $(pvs --noheading -o uuid "$dev1") )
OPVID2=$(echo $(pvs --noheading -o uuid "$dev2") )
OPVID3=$(echo $(pvs --noheading -o uuid "$dev3") )
PVID1=${OPVID1//-/}
PVID2=${OPVID2//-/}
PVID3=${OPVID3//-/}
cat "$DF"

pvs -o+uuid,deviceid "${devs[@]}" || true

cp "$ORIG" orig
sed -e "s|DEVNAME=$dev1|DEVNAME=$dev4|" orig > tmp1
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1 > tmp2
sed -e "s|PVID=$dev3|PVID=bad14onBxSiv4dot0GRDPtrWqOlr1bad|" tmp2 > "$DF"
cat "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" > out || true
cat out
grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$dev3" out |tee out3
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$OPVID3" out3
grep $vg1 out1
grep $vg2 out2
grep $vg3 out3
grep "$SERIAL1" out1
grep "$SERIAL1" out2
grep "$SERIAL3" out3

cat "$DF"
grep "$PVID1" "$DF" |tee out1
grep "$PVID2" "$DF" |tee out2
grep "$PVID3" "$DF" |tee out3
grep "$dev1" out1
grep "$SERIAL1" out1
grep "$dev2" out2
grep "$SERIAL1" out2
grep "$dev3" out3
grep "$SERIAL3" out3

# non-PV devices

aux wipefs_a "${devs[@]}"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL2" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL2" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"
echo "$SERIAL4" > "$SYS_DIR/dev/block/$MAJOR4:$MINOR4/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg4 "$dev4"
lvmdevices --adddev "$dev1"
lvmdevices --adddev "$dev2"
lvmdevices --adddev "$dev3"
cat "$DF"

grep "$dev1" "$DF" |tee out1
grep "$dev2" "$DF" |tee out2
grep "$dev3" "$DF" |tee out3
grep "$dev4" "$DF" |tee out4

grep "$SERIAL1" out1
grep "$SERIAL2" out2
grep "$SERIAL2" out3
grep "$SERIAL4" out4

pvs "${devs[@]}" > out || true
cat out
grep "$dev4" out
not grep "$dev1" out
not grep "$dev2" out
not grep "$dev3" out

pvcreate "$dev1"
pvs "${devs[@]}" > out || true
cat out
grep "$dev1" out
grep "$dev4" out
not grep "$dev2" out
not grep "$dev3" out

pvcreate "$dev2"
pvs "${devs[@]}" > out || true
cat out
grep "$dev1" out
grep "$dev4" out
grep "$dev2" out
not grep "$dev3" out

pvcreate "$dev3"
pvs "${devs[@]}" |tee out
grep "$dev1" out
grep "$dev4" out
grep "$dev2" out
grep "$dev3" out

OPVID1=$(echo $(pvs --noheading -o uuid "$dev1") )
OPVID2=$(echo $(pvs --noheading -o uuid "$dev2") )
OPVID3=$(echo $(pvs --noheading -o uuid "$dev3") )
OPVID4=$(echo $(pvs --noheading -o uuid "$dev4") )
PVID1=${OPVID1//-/}
PVID2=${OPVID2//-/}
PVID3=${OPVID3//-/}
PVID4=${OPVID4//-/}

grep "$dev1" "$DF" |tee out1
grep "$dev2" "$DF" |tee out2
grep "$dev3" "$DF" |tee out3
grep "$dev4" "$DF" |tee out4

grep "$PVID1" out1
grep "$PVID2" out2
grep "$PVID3" out3
grep "$PVID4" out4

vgcreate $vg2 "$dev2" "$dev3"
vgs | grep $vg2

# 3 devs with duplicate serial, 2 pvs with stale devnames, 1 non-pv device

aux wipefs_a "$dev1" "$dev2" "$dev3"

echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR1:$MINOR1/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR2:$MINOR2/device/serial"
echo "$SERIAL1" > "$SYS_DIR/dev/block/$MAJOR3:$MINOR3/device/serial"

rm "$DF"
touch "$DF"
vgcreate $vg1 "$dev1" "$dev2"
lvmdevices --adddev "$dev3"
cat "$DF"
cp "$DF" "$ORIG"

OPVID1=$(echo $(pvs --noheading -o uuid "$dev1") )
OPVID2=$(echo $(pvs --noheading -o uuid "$dev2") )
PVID1=${OPVID1//-/}
PVID2=${OPVID2//-/}

pvs -o+uuid,deviceid "${devs[@]}" || true

sed -e "s|DEVNAME=$dev1|DEVNAME=tmp|" "$ORIG" > tmp1
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1 > tmp2
sed -e "s|DEVNAME=tmp|DEVNAME=$dev2|" tmp2 > "$DF"
cat "$DF"

# pvs should report the correct info and fix the DF
pvs -o+uuid,deviceid "${devs[@]}" > out || true
cat out
grep "$dev1" out |tee out1
grep "$dev2" out |tee out2
grep "$OPVID1" out1
grep "$OPVID2" out2
grep "$SERIAL1" out1
grep "$SERIAL1" out2
