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

test_description='devices file with real devs'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

#
# To use this test, add two or more devices with real device ids,
# e.g. wwids, to a file, e.g.
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

if [ -z ${LVM_TEST_DEVICE_LIST+x} ]; then echo "LVM_TEST_DEVICE_LIST is unset" && skip; else echo "LVM_TEST_DEVICE_LIST is set to '$LVM_TEST_DEVICE_LIST'"; fi

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

aux lvmconf 'devices/dir = "/dev"'
aux lvmconf 'devices/use_devicesfile = 1'
DFDIR="$LVM_SYSTEM_DIR/devices"
DF="$DFDIR/system.devices"
mkdir $DFDIR || true
not ls $DF

get_real_devs

wipe_all() {
	for dev in "${REAL_DEVICES[@]}"; do
		wipefs -a $dev
	done
}

wipe_all

# check each dev is added correctly to df

touch $DF
for dev in "${REAL_DEVICES[@]}"; do
	pvcreate $dev

	pvs -o+uuid $dev
	maj=$(get pv_field "$dev" major)
	min=$(get pv_field "$dev" minor)
	pvid=`pvs $dev --noheading -o uuid | tr -d - | awk '{print $1}'`

	sys_wwid_file="/sys/dev/block/$maj:$min/device/wwid"
	sys_wwid_nvme_file="/sys/dev/block/$maj:$min/wwid"
	sys_serial_file="/sys/dev/block/$maj:$min/device/serial"
	sys_dm_uuid_file="/sys/dev/block/$maj:$min/dm/uuid"
	sys_md_uuid_file="/sys/dev/block/$maj:$min/md/uuid"
	sys_loop_file="/sys/dev/block/$maj:$min/loop/backing_file"

	if test -e $sys_wwid_file; then
		sys_file=$sys_wwid_file
		idtype="sys_wwid"
	elif test -e $sys_wwid_nvme_file; then
		sys_file=$sys_wwid_nvme_file
		idtype="sys_wwid"
	elif test -e $sys_serial_file; then
		sys_file=$sys_serial_file
		idtype="sys_serial"
	elif test -e $sys_dm_uuid_file; then
		sys_file=$sys_dm_uuid_file
		idtype="mpath_uuid"
	elif test -e $sys_md_uuid_file; then
		sys_file=$sys_md_uuid_file
		idtype="md_uuid"
	elif test -e $sys_loop_file; then
		sys_file=$sys_loop_file
		idtype="loop_file"
	else
		echo "no id type for device"
		skip
	fi

	idname=$(< $sys_file)

	rm -f idline
	grep IDNAME=$idname $DF | tee idline
	grep IDTYPE=$idtype idline
	grep DEVNAME=$dev idline
	grep PVID=$pvid idline
done

cp $DF df2

# vgcreate from existing pvs, already in df

vgcreate $vg "${REAL_DEVICES[@]}"

vgremove $vg
rm $DF

# vgcreate from existing pvs, adding to df

touch $DF
vgcreate $vg "${REAL_DEVICES[@]}"

grep IDNAME $DF > df.ids
grep IDNAME df2 > df2.ids
diff df.ids df2.ids

# check device id metadata fields

for dev in "${REAL_DEVICES[@]}"; do
	grep $dev $DF
	deviceid=`pvs $dev --noheading -o deviceid | awk '{print $1}'`
	deviceidtype=`pvs $dev --noheading -o deviceidtype | awk '{print $1}'`
	grep $dev $DF | grep $deviceid
	grep $dev $DF | grep $deviceidtype
	lvcreate -l1 $vg $dev
done

vgchange -an $vg
vgremove -y $vg

# check pvremove leaves devs in df but without pvid

for dev in "${REAL_DEVICES[@]}"; do
	maj=$(get pv_field "$dev" major)
	min=$(get pv_field "$dev" minor)
	pvid=`pvs $dev --noheading -o uuid | tr -d - | awk '{print $1}'`

	pvremove $dev
	grep $dev $DF
	not grep $pvid $DF
done

# Many of remaining tests require two or three devices
test $num_devs -gt 2 || skip

# check vgextend adds new dev to df, vgreduce leaves dev in df

rm $DF

touch $DF
vgcreate $vg $dev1
vgextend $vg $dev2
grep $dev1 $DF
grep $dev2 $DF
id1=`pvs $dev1 --noheading -o deviceid | awk '{print $1}'`
id2=`pvs $dev2 --noheading -o deviceid | awk '{print $1}'`
grep $id1 $DF
grep $id2 $DF
vgreduce $vg $dev2
grep $dev2 $DF
vgremove $vg

# check devs are not visible to lvm until added to df

rm $DF

# df needs to exist otherwise devicesfile feature turned off
touch $DF

not pvs $dev1
not pvs $dev2
pvs -a |tee all
not grep $dev1 all
not grep $dev2 all
not grep $dev1 $DF
not grep $dev2 $DF

pvcreate $dev1

pvs $dev1
not pvs $dev2
pvs -a |tee all
grep $dev1 all
not grep $dev2 all
grep $dev1 $DF
not grep $dev2 $DF

pvcreate $dev2

pvs $dev1
pvs $dev2
pvs -a |tee all
grep $dev1 all
grep $dev2 all
grep $dev1 $DF
grep $dev2 $DF

vgcreate $vg $dev1

pvs $dev1
pvs $dev2
pvs -a |tee all
grep $dev1 all
grep $dev2 all
grep $dev1 $DF
grep $dev2 $DF

vgextend $vg $dev2

pvs $dev1
pvs $dev2
pvs -a |tee all
grep $dev1 all
grep $dev2 all
grep $dev1 $DF
grep $dev2 $DF

# check vgimportdevices VG

rm $DF
wipe_all

vgcreate $vg "${REAL_DEVICES[@]}"
rm $DF
touch $DF

for dev in "${REAL_DEVICES[@]}"; do
	not pvs $dev
done

vgimportdevices $vg

for dev in "${REAL_DEVICES[@]}"; do
	pvs $dev
done

# check vgimportdevices -a

rm $DF
wipe_all

vgcreate $vg1 $dev1
vgcreate $vg2 $dev2

rm $DF

vgimportdevices -a

ls $DF

vgs $vg1
vgs $vg2

pvs $dev1
pvs $dev2

# check vgimportclone --importdevices

rm $DF
wipe_all

vgcreate $vg1 $dev1
vgimportdevices $vg1

dd if=$dev1 of=$dev2 bs=1M count=1

pvs $dev1
not pvs $dev2

grep $dev1 $DF
not grep $dev2 $DF

not vgimportclone $dev2

not grep $dev2 $DF

vgimportclone --basevgname $vg2 --importdevices $dev2

pvid1=`pvs $dev1 --noheading -o uuid | tr -d - | awk '{print $1}'`
pvid2=`pvs $dev2 --noheading -o uuid | tr -d - | awk '{print $1}'`
test "$pvid1" != "$pvid2" || die "same uuid"

test "$id1" != "$id2" || die "same device id"

grep $dev1 $DF
grep $dev2 $DF
grep $pvid1 $DF
grep $pvid2 $DF
grep $id1 $DF
grep $id2 $DF

vgs $vg1
vgs $vg2

#
# check lvmdevices
#

wipe_all
rm $DF

# set up pvs and save pvids/deviceids
touch $DF
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvcreate $dev
	vgcreate ${vg}_${count} $dev
	pvid=`pvs $dev --noheading -o uuid | tr -d - | awk '{print $1}'`
	did=`pvs $dev --noheading -o deviceid | awk '{print $1}'`
	echo dev $dev pvid $pvid did $did
	PVIDS[$count]=$pvid
	DEVICEIDS[$count]=$did
	count=$(( count + 1 ))
done

rm $DF || true
not lvmdevices
touch $DF
lvmdevices

# check lvmdevices --adddev
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvid=${PVIDS[$count]}
	did=${DEVICEIDS[$count]}
	echo $dev pvid: $pvid did: $did
	not pvs $dev
	lvmdevices --adddev $dev
	lvmdevices |tee out
	grep $dev out |tee idline
	grep $pvid idline
	grep $did idline
	grep $dev $DF
	pvs $dev
	count=$(( count + 1 ))
done

# check lvmdevices --deldev
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvid=${PVIDS[$count]}
	did=${DEVICEIDS[$count]}
	pvs $dev
	lvmdevices --deldev $dev
	lvmdevices |tee out
	not grep $dev out
	not grep $pvid out
	not grep $did out
	not grep $dev $DF
	not pvs $dev
	count=$(( count + 1 ))
done

# check lvmdevices --addpvid
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvid=${PVIDS[$count]}
	did=${DEVICEIDS[$count]}
	not pvs $dev
	lvmdevices --addpvid $pvid
	lvmdevices |tee out
	grep $dev out |tee idline
	grep $pvid idline
	grep $did idline
	grep $dev $DF
	pvs $dev
	count=$((  count + 1 ))
done

# check lvmdevices --delpvid
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvid=${PVIDS[$count]}
	did=${DEVICEIDS[$count]}
	pvs $dev
	lvmdevices --delpvid $pvid
	lvmdevices |tee out
	not grep $dev out
	not grep $pvid out
	not grep $did out
	not grep $dev $DF
	not pvs $dev
	count=$((  count + 1 ))
done

# wrong pvid in df
rm $DF
pvid1=${PVIDS[0]}
pvid2=${PVIDS[1]}
did1=${DEVICEIDS[0]}
did2=${DEVICEIDS[1]}
lvmdevices --adddev $dev1
lvmdevices --adddev $dev2

# test bad pvid
cp $DF $DF.orig
rm $DF
sed "s/$pvid1/badpvid/" "$DF.orig" |tee $DF
not grep $pvid1 $DF
grep $did1 $DF

not lvmdevices --check 2>&1|tee out
grep $dev1 out
grep badpvid out
grep $pvid1 out
not grep $dev2 out

lvmdevices |tee out
grep $dev1 out |tee out1
grep badpvid out1
not grep $pvid1 out1
grep $dev2 out

lvmdevices --update

lvmdevices 2>&1|tee out
grep $dev1 out
grep $dev2 out
not grep badpvid
grep $pvid1 out
grep $did1 out
grep $pvid1 $DF
grep $did1 $DF

# wrong deviceid in df
# the devicesfile logic and behavior is based on the idname being
# the primary identifier that we trust over everything else, i.e.
# we'll never assume that the deviceid is wrong and some other
# field is correct, and "fix" the deviceid.  We always assume the
# deviceid correct and other values are wrong (since pvid and devname
# have known, common ways of becoming wrong, but the deviceid doesn't
# really have any known way of becoming wrong apart from random
# file corruption.)
# So, if the deviceid *is* corrupted, as we do here, then standard
# commands won't correct it.  We need to use delpvid/addpvid explicitly
# to say that we are targeting the given pvid.

rm $DF
sed "s/$did1/baddid/" "$DF.orig" |tee $DF

lvmdevices --check 2>&1|tee out
grep $dev1 out
grep baddid out
not grep $dev2 out

lvmdevices 2>&1|tee out
grep $pvid1 out
grep $pvid2 out
grep baddid out
grep $did2 out
grep $dev2 out

lvmdevices --delpvid $pvid1
lvmdevices --addpvid $pvid1

lvmdevices |tee out
grep $dev1 out
grep $dev2 out
not grep baddid
grep $pvid1 out
grep $did1 out
grep $pvid1 $DF
grep $did1 $DF

# wrong devname in df, this is expected to become incorrect regularly
# given inconsistent dev names after reboot

rm $DF
d1=$(basename $dev1)
d3=$(basename $dev3)
sed "s/$d1/$d3/" "$DF.orig" |tee $DF
not lvmdevices --check 2>&1 |tee out
grep $dev1 out

lvmdevices --update

lvmdevices |tee out
grep $dev1 out |tee out1
grep $pvid1 out1
grep $did1 out1
grep $dev2 out |tee out2
grep $pvid2 out2
grep $did2 out2

# swap devnames for two existing entries

rm $DF
d1=$(basename $dev1)
d2=$(basename $dev2)
sed "s/$d1/tmp/" "$DF.orig" |tee ${DF}_1
sed "s/$d2/$d1/" "${DF}_1" |tee ${DF}_2
sed "s/tmp/$d2/" "${DF}_2" |tee $DF
rm ${DF}_1 ${DF}_2
not lvmdevices --check 2>&1 |tee out
grep $dev1 out
grep $dev2 out

lvmdevices --update

lvmdevices |tee out
grep $dev1 out |tee out1
grep $pvid1 out1
grep $did1 out1
grep $dev2 out |tee out2
grep $pvid2 out2
grep $did2 out2

# ordinary command is not confused by wrong devname and fixes
# the wrong devname in df

rm $DF
d1=$(basename $dev1)
d3=$(basename $dev3)
sed "s/$d1/$d3/" "$DF.orig" |tee $DF
not lvmdevices --check 2>&1 |tee out
grep $dev1 out

pvs -o+uuid,deviceid | grep $vg |tee out
grep $dev1 out |tee out1
grep $dev2 out |tee out2
grep $did1 out1
grep $did2 out2
not grep $dev3 out

# same dev info reported after df is fixed
pvs -o+uuid,deviceid | grep $vg |tee out3
diff out out3

pvid=`pvs $dev1 --noheading -o uuid | tr -d - | awk '{print $1}'`
test "$pvid" == "$pvid1" || die "wrong uuid"
pvid=`pvs $dev2 --noheading -o uuid | tr -d - | awk '{print $1}'`
test "$pvid" == "$pvid2" || die "wrong uuid"

lvmdevices |tee out
grep $dev1 out |tee out1
grep $pvid1 out1
grep $did1 out1
grep $dev2 out |tee out2
grep $pvid2 out2
grep $did2 out2

# pvscan --cache doesn't fix wrong devname but still works correctly with
# the correct device

wipe_all
rm $DF
touch $DF
vgcreate $vg $dev1 $dev2
vgcreate $vg3 $dev3
lvcreate -an -n $lv1 -l1 $vg $dev1
lvcreate -an -n $lv2 -l1 $vg $dev2
lvcreate -an -n $lv3 -l1 $vg3 $dev3
PVID1=`pvs $dev1 --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID2=`pvs $dev2 --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID3=`pvs $dev3 --noheading -o uuid | tr -d - | awk '{print $1}'`
rm $DF
lvmdevices --adddev $dev1
lvmdevices --adddev $dev2
cp $DF $DF.orig
d1=$(basename $dev1)
d3=$(basename $dev3)
sed "s/$d1/$d3/" "$DF.orig" |tee $DF
_clear_online_files
pvscan --cache -aay $dev1
pvscan --cache -aay $dev2
# pvscan should ignore dev3 since it's not in DF
pvscan --cache -aay $dev3
# pvscan does not fix the devname field in DF
grep $dev3 $DF
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"
check lv_field $vg/$lv1 lv_active "active"
check lv_field $vg/$lv2 lv_active "active"
# pvs updates the DF
pvs |tee out
grep $dev1 out
grep $dev2 out
not grep $dev3 out
grep $dev1 $DF
grep $dev2 $DF
not grep $dev3 $DF
not pvs $dev3
vgchange -an $vg
wipe_all

