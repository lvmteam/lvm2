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

test_description='devices file'

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_devs 7

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

wipe_all() {
	aux wipefs_a "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7"
}

# The tests run with system dir of "/etc" but lvm when running
# normally has cmd->system_dir set to "/etc/lvm".
DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir -p "$DFDIR" || true
DF="$DFDIR/system.devices"

#
# Test with use_devicesfile=0 (no devices file is being applied by default)
#

aux lvmconf 'devices/use_devicesfile = 0'

wipe_all
rm -f "$DF"
pvcreate "$dev1"
not ls "$DF"

wipe_all
rm -f "$DF"
vgcreate $vg1 "$dev1"
not ls "$DF"

wipe_all
rm -f "$DF"

# create one VG in a non-system devices file
vgcreate --devicesfile test.devices $vg1 "$dev1"
vgextend --devicesfile test.devices $vg1 "$dev2"
cat "$DFDIR/test.devices"
grep "$dev1" "$DFDIR/test.devices"
grep "$dev2" "$DFDIR/test.devices"
not ls "$DFDIR/system.devices"

# create two VGs outside the special devices file
vgcreate $vg2 "$dev3" "$dev4"
vgcreate $vg3 "$dev5" "$dev6"
not grep "$dev3" "$DFDIR/test.devices"
not grep "$dev5" "$DFDIR/test.devices"
not ls "$DFDIR/system.devices"

PVID1=`pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID2=`pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID3=`pvs "$dev3" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID4=`pvs "$dev4" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID5=`pvs "$dev5" --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID6=`pvs "$dev6" --noheading -o uuid | tr -d - | awk '{print $1}'`

lvcreate -l4 -an -i2 -n $lv1 $vg1
lvcreate -l4 -an -i2 -n $lv2 $vg2
lvcreate -l4 -an -i2 -n $lv3 $vg3

cat "$DFDIR/test.devices"
grep "$PVID1" "$DFDIR/test.devices"
grep "$PVID2" "$DFDIR/test.devices"
not grep "$PVID3" "$DFDIR/test.devices"
not grep "$PVID4" "$DFDIR/test.devices"
not grep "$PVID5" "$DFDIR/test.devices"
not grep "$PVID6" "$DFDIR/test.devices"
not ls "$DFDIR/system.devices"

# verify devices file is working
vgs --devicesfile test.devices $vg1
not vgs --devicesfile test.devices $vg2

# misspelled override name fails
not vgs --devicesfile doesnotexist $vg1
not vgs --devicesfile doesnotexist $vg2
not vgs --devicesfile doesnotexist

# devicesfile and devices cannot be used together
not vgs --devicesfile test.devices --devices "$dev1","$dev1" $vg1

# verify correct vgs are seen / not seen when devices are specified
vgs --devices "$dev1","$dev2" $vg1
vgs --devices "$dev3","$dev4" $vg2
vgs --devices "$dev5","$dev6" $vg3
not vgs --devices "$dev1","$dev2" $vg2
not vgs --devices "$dev1","$dev2" $vg3
not vgs --devices "$dev1","$dev2" $vg2
not vgs --devices "$dev5","$dev6" $vg2
not vgs --devices "$dev1","$dev2" $vg3
not vgs --devices "$dev3","$dev4" $vg3

vgs --devices "$dev1","$dev2" |tee out
grep $vg1 out
not grep $vg2 out
not grep $vg3 out
vgs --devices "$dev3","$dev4" |tee out
not grep $vg1 out
grep $vg2 out
not grep $vg3 out

# verify correct pvs are seen / not seen when devices are specified
pvs --devices "$dev1","$dev2" "$dev1" "$dev2"
pvs --devices "$dev3","$dev4" "$dev3" "$dev4"
pvs --devices "$dev5","$dev6" "$dev5" "$dev6"
not pvs --devices "$dev1","$dev2" "$dev3" "$dev4"
not pvs --devices "$dev1","$dev2" "$dev5" "$dev6"
not pvs --devices "$dev3","$dev4" "$dev1" "$dev2" "$dev5" "$dev6"

pvs --devices "$dev1","$dev2" |tee out
grep "$dev1" out
grep "$dev2" out
not grep "$dev3" out
not grep "$dev4" out
not grep "$dev5" out
not grep "$dev6" out
pvs --devices "$dev3","$dev4" |tee out
not grep "$dev1" out
not grep "$dev2" out
grep "$dev3" out
grep "$dev4" out
not grep "$dev5" out
not grep "$dev6" out

# verify correct lvs are activated / not activated when devices are specified
vgchange --devices "$dev1","$dev2" -ay
check lv_field $vg1/$lv1 lv_active "active"
check lv_field $vg2/$lv2 lv_active ""
check lv_field $vg3/$lv3 lv_active ""
vgchange --devices "$dev1","$dev2" -an
check lv_field $vg1/$lv1 lv_active ""

vgchange --devices "$dev3","$dev4" -ay
check lv_field $vg1/$lv1 lv_active ""
check lv_field $vg2/$lv2 lv_active "active"
check lv_field $vg3/$lv3 lv_active ""
vgchange --devices "$dev3","$dev4" -an
check lv_field $vg2/$lv2 lv_active ""

# verify devices covering multiple vgs
vgs --devices "$dev1","$dev2","$dev3","$dev4" $vg1 $vg2 |tee out
grep $vg1 out
grep $vg2 out
not grep $vg3 out
vgs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" $vg1 $vg2 $vg3 |tee out
grep $vg1 out
grep $vg2 out
grep $vg3 out

# verify vgs seen when incomplete devices are specified
vgs --devices "$dev1" $vg1
vgs --devices "$dev3" $vg2
vgs --devices "$dev5" $vg3

# incomplete vg because of --devices is the same as vg incomplete because
# of missing device
not lvcreate --devices "$dev1" -l1 $vg1
not lvchange --devices "$dev1" -ay $vg1/$lv1
not lvextend --devices "$dev1" -l+1 $vg1/$lv1
not vgremove --devices "$dev1" $vg1
not lvcreate --devices "$dev3" -l1 $vg2
not lvchange --devices "$dev3" -ay $vg2/$lv2
not lvextend --devices "$dev3" -l+1 $vg2/$lv2
not vgremove --devices "$dev3" $vg2

# verify various commands with --devices for vg in a devicesfile
not lvcreate --devices "$dev1","$dev2" -l1 -n $lv2 -an $vg1 "$dev7"
lvcreate --devices "$dev1","$dev2" -l1 -n $lv2 -an $vg1
lvs --devices "$dev1","$dev2" $vg1/$lv2
lvextend --devices "$dev1","$dev2" -l2 $vg1/$lv2
lvchange --devices "$dev1","$dev2" -ay $vg1/$lv2
lvchange --devices "$dev1","$dev2" -an $vg1/$lv2
lvremove --devices "$dev1","$dev2" $vg1/$lv2
vgchange --devices "$dev1","$dev2" -ay $vg1
vgchange --devices "$dev1","$dev2" -an $vg1
not vgextend --devices "$dev1","$dev2" $vg1 "$dev7"
vgextend --devices "$dev1","$dev2","$dev7" $vg1 "$dev7"
vgreduce --devices "$dev1","$dev2","$dev7" $vg1 "$dev7"
vgexport --devices "$dev1","$dev2" $vg1
vgimport --devices "$dev1","$dev2" $vg1
not pvremove --devices "$dev1","$dev2" "$dev7"
not pvcreate --devices "$dev1","$dev2" "$dev7"
not vgcreate --devices "$dev1","$dev2" $vg7 "$dev7"
pvremove --devices "$dev7" "$dev7"
pvcreate --devices "$dev7" "$dev7"
vgcreate --devices "$dev7" $vg7 "$dev7"
vgremove --devices "$dev7" $vg7
pvremove --devices "$dev7" "$dev7"

# verify various commands with --devices for vg not in a devicesfile
not lvcreate --devices "$dev3","$dev4" -l1 -n $lv4 -an $vg2 "$dev7"
lvcreate --devices "$dev3","$dev4" -l1 -n $lv4 -an $vg2
lvs --devices "$dev3","$dev4" $vg2/$lv4
lvextend --devices "$dev3","$dev4" -l2 $vg2/$lv4
lvchange --devices "$dev3","$dev4" -ay $vg2/$lv4
lvchange --devices "$dev3","$dev4" -an $vg2/$lv4
lvremove --devices "$dev3","$dev4" $vg2/$lv4
vgchange --devices "$dev3","$dev4" -ay $vg2
vgchange --devices "$dev3","$dev4" -an $vg2
not vgextend --devices "$dev3","$dev4" $vg2 "$dev7"
vgextend --devices "$dev3","$dev4","$dev7" $vg2 "$dev7"
vgreduce --devices "$dev3","$dev4","$dev7" $vg2 "$dev7"
vgexport --devices "$dev3","$dev4" $vg2
vgimport --devices "$dev3","$dev4" $vg2
not pvremove --devices "$dev3","$dev4" "$dev7"
not pvcreate --devices "$dev3","$dev4" "$dev7"
not vgcreate --devices "$dev3","$dev4" $vg7 "$dev7"
pvremove --devices "$dev7" "$dev7"
pvcreate --devices "$dev7" "$dev7"
vgcreate --devices "$dev7" $vg7 "$dev7"
vgremove --devices "$dev7" $vg7
pvremove --devices "$dev7" "$dev7"

# verify pvscan with devices file and devices list

# arg not in devices file
_clear_online_files
pvscan --devicesfile test.devices --cache -aay "$dev3"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"
pvscan --devicesfile test.devices --cache -aay "$dev4"
not ls "$RUNDIR/lvm/pvs_online/$PVID4"
check lv_field $vg1/$lv1 lv_active ""
check lv_field $vg2/$lv2 lv_active ""

# arg in devices file
_clear_online_files
pvscan --devicesfile test.devices --cache "$dev1"
pvscan --devicesfile test.devices --cache "$dev2"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"

# autoactivate with devices file
_clear_online_files
pvscan --devicesfile test.devices --cache -aay "$dev1"
pvscan --devicesfile test.devices --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
vgchange -an $vg1

# autoactivate with no devices file
_clear_online_files
pvscan --cache -aay "$dev3"
pvscan --cache -aay "$dev4"
check lv_field $vg2/$lv2 lv_active "active"
vgchange -an $vg2

# arg not in devices list
_clear_online_files
pvscan --devices "$dev1","$dev2" --cache "$dev3"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"
pvscan --devices "$dev4" --cache "$dev3"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"
pvscan --devices "$dev5" --cache "$dev3"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"

# arg in devices list
_clear_online_files
pvscan --devices "$dev3" --cache -aay "$dev3"
pvscan --devices "$dev4","$dev3" --cache -aay "$dev4"
check lv_field $vg2/$lv2 lv_active "active"
vgchange -an $vg2

vgchange --devicesfile "" -an
vgremove --devicesfile "" -y $vg1
vgremove --devicesfile "" -y $vg2
vgremove --devicesfile "" -y $vg3

#
# Test with use_devicesfile=1 (system devices file is in use by default)
#

aux lvmconf 'devices/use_devicesfile = 1'

DF="$DFDIR/system.devices"
touch "$DF"

# create one VG in a non-system devices file
vgcreate --devicesfile test.devices $vg1 "$dev1" "$dev2"

# create one VG in the default system devices file
vgcreate $vg2 "$dev3" "$dev4"

# create one VG in neither devices file
vgcreate --devicesfile "" $vg3 "$dev5" "$dev6"

lvcreate --devicesfile test.devices -l4 -an -i2 -n $lv1 $vg1
lvcreate -l4 -an -i2 -n $lv2 $vg2
lvcreate --devicesfile "" -l4 -an -i2 -n $lv3 $vg3

# system.devices only sees vg2
vgs |tee out
not grep $vg1 out
grep $vg2 out
not grep $vg3 out
not vgs $vg1
vgs $vg2
not vgs $vg3
pvs |tee out
not grep "$dev1" out
not grep "$dev2" out
grep "$dev3" out
grep "$dev4" out
not grep "$dev5" out
not grep "$dev6" out

# test.devices only sees vg1
vgs --devicesfile test.devices |tee out
grep $vg1 out
not grep $vg2 out
not grep $vg3 out
pvs --devicesfile test.devices |tee out
grep "$dev1" out
grep "$dev2" out
not grep "$dev3" out
not grep "$dev4" out
not grep "$dev5" out
not grep "$dev6" out

# no devices file sees all
vgs --devicesfile "" |tee out
grep $vg1 out
grep $vg2 out
grep $vg3 out
vgs --devicesfile "" $vg1
vgs --devicesfile "" $vg2
vgs --devicesfile "" $vg3
pvs --devicesfile "" |tee out
grep "$dev1" out
grep "$dev2" out
grep "$dev3" out
grep "$dev4" out
grep "$dev5" out
grep "$dev6" out

vgchange -ay
lvs --devicesfile test.devices -o active $vg1/$lv1 |tee out
not grep active out
lvs -o active $vg2/$lv2 |tee out
grep active out
lvs --devicesfile "" -o active $vg3/$lv3 |tee out
not grep active out
vgchange -an
lvs -o active $vg2/$lv2 |tee out
not grep active out

vgchange --devicesfile test.devices -ay
lvs --devicesfile test.devices -o active $vg1/$lv1 |tee out
grep active out
lvs -o active $vg2/$lv2 |tee out
not grep active out
lvs --devicesfile "" -o active $vg3/$lv3 |tee out
not grep active out
vgchange --devicesfile test.devices -an
lvs --devicesfile test.devices -o active $vg1/$lv1 |tee out
not grep active out

# --devices overrides all three cases:
# always gives access to the specified devices
# always denies access to unspecified devices

vgs --devices "$dev1","$dev2" $vg1
vgs --devices "$dev3","$dev4" $vg2
vgs --devices "$dev5","$dev6" $vg3

pvs --devices "$dev1" "$dev1"
pvs --devices "$dev3" "$dev3"
pvs --devices "$dev5" "$dev5"

not pvs --devices "$dev1" "$dev1" "$dev2" |tee out
grep "$dev1" out
not grep "$dev2" out

not pvs --devices "$dev3" "$dev3" "$dev4" |tee out
grep "$dev3" out
not grep "$dev4" out

not pvs --devices "$dev5" "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" |tee out
grep "$dev5" out
not grep "$dev1" out
not grep "$dev2" out
not grep "$dev3" out
not grep "$dev4" out
not grep "$dev6" out

pvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5" "$dev5" |tee out
grep "$dev5" out
not grep "$dev1" out
not grep "$dev2" out
not grep "$dev3" out
not grep "$dev4" out
not grep "$dev6" out

pvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5" "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" |tee out
grep "$dev1" out
grep "$dev2" out
grep "$dev3" out
grep "$dev4" out
grep "$dev5" out

vgchange --devices "$dev1","$dev2" -ay
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | grep active |tee out
grep $lv1 out
not grep $lv2 out
not grep $lv3 out
vgchange --devices "$dev1","$dev2" -an
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | tee out
not grep active out

vgchange --devices "$dev3","$dev4" -ay
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | grep active |tee out
not grep $lv1 out
grep $lv2 out
not grep $lv3 out
vgchange --devices "$dev3","$dev4" -an
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active |tee out
not grep active out

vgchange --devices "$dev5","$dev6" -ay
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | grep active |tee out
not grep $lv1 out
not grep $lv2 out
grep $lv3 out
vgchange --devices "$dev5","$dev6" -an
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active |tee out
not grep active out

lvcreate --devices "$dev1","$dev2" -l1 -an -n $lv4 $vg1
lvremove --devices "$dev1","$dev2" $vg1/$lv4
lvcreate --devices "$dev3","$dev4" -l1 -an -n $lv4 $vg2
lvremove --devices "$dev3","$dev4" $vg2/$lv4
lvcreate --devices "$dev5","$dev6" -l1 -an -n $lv4 $vg3
lvremove --devices "$dev5","$dev6" $vg3/$lv4

not vgchange --devices "$dev1","$dev2" -ay $vg2
not vgchange --devices "$dev1","$dev2" -ay $vg3
not vgchange --devices "$dev3","$dev4" -ay $vg1
not vgchange --devices "$dev3","$dev4" -ay $vg3
not vgchange --devices "$dev5","$dev6" -ay $vg1
not vgchange --devices "$dev5","$dev6" -ay $vg2

not lvcreate --devices "$dev1","$dev2" -an -l1 $vg2
not lvcreate --devices "$dev1","$dev2" -an -l1 $vg3
not lvcreate --devices "$dev3","$dev4" -an -l1 $vg1
not lvcreate --devices "$dev3","$dev4" -an -l1 $vg3
not lvcreate --devices "$dev5","$dev6" -an -l1 $vg1
not lvcreate --devices "$dev5","$dev6" -an -l1 $vg2

# autoactivate devs in default devices file
_clear_online_files
pvscan --cache -aay "$dev3"
pvscan --cache -aay "$dev4"
check lv_field $vg2/$lv2 lv_active "active"
vgchange -an $vg2
pvscan --cache -aay "$dev1"
not ls "$RUNDIR/lvm/pvs_online/$PVID1"
pvscan --cache -aay "$dev2"
not ls "$RUNDIR/lvm/pvs_online/$PVID2"
pvscan --cache -aay "$dev5"
not ls "$RUNDIR/lvm/pvs_online/$PVID5"
_clear_online_files
pvscan --devices "$dev3" --cache -aay "$dev3"
pvscan --devices "$dev3","$dev4" --cache -aay "$dev4"
lvs --devices "$dev3","$dev4" -o active $vg2/$lv2 | grep active
vgchange --devices "$dev3","$dev4" -an $vg2

not vgchange -ay $vg1
vgchange --devicesfile test.devices -ay $vg1
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | grep active |tee out
grep $lv1 out
not grep $lv2 out
not grep $lv3 out

vgchange -ay $vg2
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | grep active |tee out
grep $lv1 out
grep $lv2 out
not grep $lv3 out

not vgchange -ay $vg3
vgchange --devicesfile "" -ay $vg3
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | grep active |tee out
grep $lv1 out
grep $lv2 out
grep $lv3 out

vgchange -an
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | grep active |tee out
grep $lv1 out
not grep $lv2 out
grep $lv3 out

vgchange -ay
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active | grep active |tee out
grep $lv1 out
grep $lv2 out
grep $lv3 out

vgchange --devicesfile "" -an
lvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5","$dev6" -o name,active |tee out
not grep active out

not vgremove $vg1
not vgremove $vg3
vgremove -y $vg2
vgremove --devicesfile test.devices -y $vg1
vgremove --devicesfile "" -y $vg3

#
# Test when system.devices is created by lvm
#

# no pvs exist, pvcreate creates DF, e.g. system installation

wipe_all
rm -f "$DF"
pvcreate "$dev1"
ls "$DF"
grep "$dev1" "$DF"

# no pvs exist, vgcreate creates DF, e.g. system installation

wipe_all
rm -f "$DF"
vgcreate $vg1 "$dev1"
ls "$DF"
grep "$dev1" "$DF"

# no pvs exist, touch DF, pvcreate uses it

wipe_all
rm -f "$DF"
touch "$DF"
pvcreate "$dev1"
grep "$dev1" "$DF"

# no vgs exist, touch DF, vgcreate uses it

wipe_all
rm -f "$DF"
touch "$DF"
vgcreate $vg1 "$dev1"
grep "$dev1" "$DF"

# vgs exist, pvcreate/vgcreate do not create DF

wipe_all
rm -f "$DF"
vgcreate $vg1 "$dev1"
ls "$DF"
rm "$DF"
pvcreate "$dev2"
not ls "$DF"
vgcreate $vg3 "$dev3"
not ls "$DF"

# vgs exist, pvcreate/vgcreate --devicesfile system.devices creates DF

wipe_all
rm -f "$DF"
vgcreate $vg1 "$dev1"
ls "$DF"
rm "$DF"
pvcreate --devicesfile system.devices "$dev2"
ls "$DF"
grep "$dev2" "$DF"
rm "$DF"
vgcreate --devicesfile system.devices $vg3 "$dev3"
ls "$DF"
grep "$dev3" "$DF"

# pvcreate/vgcreate always create non-system DF if it doesn't exist

wipe_all
rm -f "$DF"
vgcreate $vg1 "$dev1"
rm "$DF"
rm "$DFDIR/test.devices"
pvcreate --devicesfile test.devices "$dev2"
grep "$dev2" "$DFDIR/test.devices"
rm "$DFDIR/test.devices"
vgcreate --devicesfile test.devices $vg3 "$dev3"
grep "$dev3" "$DFDIR/test.devices"

# vgchange uuid handles stacked PVs on VGs

wipe_all
rm -f "$DF"
vgcreate $vg1 "$dev1"
lvcreate -l8 -n $lv1 $vg1
aux lvmconf 'devices/scan_lvs = 1'
pvcreate "$DM_DEV_DIR/$vg1/$lv1"
pvs "$DM_DEV_DIR/$vg1/$lv1"
grep "$DM_DEV_DIR/$vg1/$lv1" $DF
vgchange -an $vg1
vgchange --uuid $vg1
vgchange -ay $vg1
pvs "$DM_DEV_DIR/$vg1/$lv1"
vgchange -an $vg1
not pvs "$DM_DEV_DIR/$vg1/$lv1"
aux lvmconf 'devices/scan_lvs = 0'
vgremove -y $vg1

#
# verify --devicesfile and --devices are not affected by a filter
# This is last because it sets lvm.conf filter and
# I haven't found a way of removing the filter from
# the config after setting it.
#

aux lvmconf 'devices/use_devicesfile = 0'
wipe_all
rm -f "$DF"
rm -f "$DFDIR/test.devices"

vgcreate --devicesfile test.devices $vg1 "$dev1" "$dev2"
grep "$dev1" "$DFDIR/test.devices"
grep "$dev2" "$DFDIR/test.devices"
not ls "$DFDIR/system.devices"

# create two VGs outside the special devices file
vgcreate $vg2 "$dev3" "$dev4"
vgcreate $vg3 "$dev5" "$dev6"
not grep "$dev3" "$DFDIR/test.devices"
not grep "$dev5" "$DFDIR/test.devices"
not ls "$DFDIR/system.devices"

lvcreate -l4 -an -i2 -n $lv1 $vg1
lvcreate -l4 -an -i2 -n $lv2 $vg2
lvcreate -l4 -an -i2 -n $lv3 $vg3

aux lvmconf "devices/filter = [ \"r|$dev2|\" \"r|$dev4|\" ]"

pvs --devicesfile test.devices "$dev1"
pvs --devicesfile test.devices "$dev2"
not pvs --devicesfile test.devices "$dev3"
not pvs --devicesfile test.devices "$dev4"
pvs --devices "$dev1" "$dev1"
pvs --devices "$dev2" "$dev2"
pvs --devices "$dev3" "$dev3"
pvs --devices "$dev4" "$dev4"
pvs --devices "$dev5" "$dev5"
pvs --devices "$dev1","$dev2","$dev3","$dev4","$dev5" "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" | tee out
grep "$dev1" out
grep "$dev2" out
grep "$dev3" out
grep "$dev4" out
grep "$dev5" out
vgchange --devices "$dev1","$dev2" -ay $vg1
check lv_field $vg1/$lv1 lv_active "active"
lvchange --devices "$dev1","$dev2" -an $vg1/$lv1
vgchange --devices "$dev3","$dev4" -ay $vg2
check lv_field $vg2/$lv2 lv_active "active"
lvchange --devices "$dev3","$dev4" -an $vg2/$lv2

vgchange -an --devicesfile test.devices $vg1
vgremove -y --devicesfile test.devices $vg1
vgremove -y $vg2
vgremove -y $vg3

