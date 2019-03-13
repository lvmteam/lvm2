#!/usr/bin/env bash

# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
PVS_ONLINE_DIR="$RUNDIR/lvm/pvs_online"
VGS_ONLINE_DIR="$RUNDIR/lvm/vgs_online"

# FIXME: kills logic for running system
_clear_online_files() {
	# wait till udev is finished
	aux udev_wait
	rm -f "$PVS_ONLINE_DIR"/*
	rm -f "$VGS_ONLINE_DIR"/*
}

. lib/inittest

aux prepare_pvs 3

vgcreate $vg1 "$dev1" "$dev2"
lvcreate -n $lv1 -l 4 -a n $vg1

# the first pvscan scans all devs
test -d "$PVS_ONLINE_DIR" || mkdir -p "$PVS_ONLINE_DIR"
test -d "$VGS_ONLINE_DIR" || mkdir -p "$VGS_ONLINE_DIR"
_clear_online_files

pvscan --cache -aay
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# the first pvscan scans all devs even when
# only one device is specified

_clear_online_files

pvscan --cache -aay "$dev1"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# touch foo to disable first-pvscan case,
# then check pvscan with no args scans all
_clear_online_files
touch "$RUNDIR/lvm/pvs_online/foo"

pvscan --cache -aay
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# touch foo to disable first-pvscan case,
# then check that vg is activated only after
# both devs appear separately

_clear_online_files
touch "$RUNDIR/lvm/pvs_online/foo"

pvscan --cache -aay "$dev1"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# touch foo to disable first-pvscan case,
# then check that vg is activated when both
# devs appear together

_clear_online_files
touch "$RUNDIR/lvm/pvs_online/foo"

pvscan --cache -aay "$dev1" "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# Set up tests where one dev has no metadata

vgchange -an $vg1
vgremove -ff $vg1
pvremove "$dev1"
pvremove "$dev2"
pvcreate --metadatacopies 0 "$dev1"
pvcreate --metadatacopies 1 "$dev2"
vgcreate $vg1 "$dev1" "$dev2"
lvcreate -n $lv1 -l 4 -a n $vg1

# touch foo to disable first-pvscan case,
# test case where dev with metadata appears first

_clear_online_files
touch "$RUNDIR/lvm/pvs_online/foo"

pvscan --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache -aay "$dev1"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# touch foo to disable first-pvscan case,
# test case where dev without metadata
# appears first which triggers scanning all

_clear_online_files
touch "$RUNDIR/lvm/pvs_online/foo"

pvscan --cache -aay "$dev1"
check lv_field $vg1/$lv1 lv_active "active"
pvscan --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# dev without metadata is scanned, but
# first-pvscan case scans all devs

_clear_online_files

pvscan --cache -aay "$dev1"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# use the --cache option to record a dev
# is online without the -aay option to
# activate until after they are online

_clear_online_files
touch "$RUNDIR/lvm/pvs_online/foo"

pvscan --cache "$dev1"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache "$dev2"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache -aay
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# like previous

_clear_online_files
touch "$RUNDIR/lvm/pvs_online/foo"

pvscan --cache "$dev1"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

vgremove -f $vg1

# pvscan cache ignores pv that's not used

pvcreate "$dev3"

PVID3=`pvs $dev3 --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVID3

not ls "$RUNDIR/lvm/pvs_online/$PVID3"

pvscan --cache -aay "$dev3"

ls "$RUNDIR/lvm/pvs_online"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"


# pvscan cache ignores pv in a foreign vg

aux lvmconf "global/system_id_source = uname"

_clear_online_files

vgcreate $vg2 "$dev3"
lvcreate -an -n $lv2 -l1 $vg2
pvscan --cache -aay "$dev3"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
check lv_field $vg2/$lv2 lv_active "active"
lvchange -an $vg2
rm "$RUNDIR/lvm/pvs_online/$PVID3"

# a vg without a system id is not foreign, not ignored
vgchange -y --systemid "" "$vg2"

_clear_online_files
pvscan --cache -aay "$dev3"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
check lv_field $vg2/$lv2 lv_active "active"
lvchange -an $vg2
rm "$RUNDIR/lvm/pvs_online/$PVID3"

# make the vg foreign by assigning a system id different from ours
vgchange -y --systemid "asdf" "$vg2"

_clear_online_files

pvscan --cache -aay "$dev3"
not ls "$RUNDIR/lvm/pvs_online/$PVID3"
lvs --foreign $vg2 > tmp
cat tmp
grep $lv2 tmp
check lv_field $vg2/$lv2 lv_active "" --foreign



