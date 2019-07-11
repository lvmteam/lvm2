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

aux prepare_devs 8 16

vgcreate $vg1 "$dev1" "$dev2"
lvcreate -n $lv1 -l 4 -a n $vg1

test -d "$PVS_ONLINE_DIR" || mkdir -p "$PVS_ONLINE_DIR"
test -d "$VGS_ONLINE_DIR" || mkdir -p "$VGS_ONLINE_DIR"
_clear_online_files

# check pvscan with no args scans and activates all
pvscan --cache -aay
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

_clear_online_files

# first dev leaves vg incomplete and inactive,
# and second dev completes vg and activates
pvscan --cache -aay "$dev1"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

_clear_online_files

# check that vg is activated when both devs
# are scanned together
pvscan --cache -aay "$dev1" "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# check that a cache command without aay will
# just record online state, and that a following
# pvscan cache aay that does not record any new
# online files will activate the vg
_clear_online_files
pvscan --cache "$dev1"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache "$dev2"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache -aay
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


_clear_online_files

# test case where dev with metadata is scanned first
pvscan --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache -aay "$dev1"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# test case where dev without metadata is scanned first
# which triggers scanning all, which finds both

_clear_online_files
pvscan --cache -aay "$dev1"
check lv_field $vg1/$lv1 lv_active "active"
pvscan --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an $vg1

# use the --cache option to record a dev
# is online without the -aay option to
# activate until after they are online

_clear_online_files

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

if [ -e "/etc/machine-id" ]; then

aux lvmconf "global/system_id_source = machineid"

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

fi


# Test the case where pvscan --cache -aay (with no devs)
# gets the final PV to complete the VG, where that final PV
# does not hold VG metadata.  In this case it needs to rely
# on VG metadata that has been saved from a previously
# scanned PV from the same VG.
#
# We can't control which order of devices pvscan will see,
# so create several PVs without metadata surrounding one
# PV with metadata, to make it likely that pvscan will
# get a final PV without metadata.

pvcreate --metadatacopies 0 "$dev4"
pvcreate --metadatacopies 0 "$dev5"
pvcreate --metadatacopies 1 "$dev6"
pvcreate --metadatacopies 0 "$dev7"
pvcreate --metadatacopies 0 "$dev8"
vgcreate $vg3 "$dev4" "$dev5" "$dev6" "$dev7" "$dev8"
lvcreate -n $lv1 -l 4 -a n $vg3

_clear_online_files

check lv_field $vg3/$lv1 lv_active ""
pvscan --cache "$dev4"
check lv_field $vg3/$lv1 lv_active ""
pvscan --cache "$dev5"
check lv_field $vg3/$lv1 lv_active ""
pvscan --cache "$dev6"
check lv_field $vg3/$lv1 lv_active ""
pvscan --cache "$dev7"
check lv_field $vg3/$lv1 lv_active ""
pvscan --cache "$dev8"
check lv_field $vg3/$lv1 lv_active ""
pvscan --cache -aay
check lv_field $vg3/$lv1 lv_active "active"
lvchange -an $vg3

# Test event activation when PV and dev size don't match

vgremove -ff $vg3

pvremove "$dev8"
pvcreate -y --setphysicalvolumesize 8M "$dev8"

PVID8=`pvs $dev8 --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVID8

vgcreate $vg3 "$dev8"
lvcreate -l1 -n $lv1 $vg3
check lv_field $vg3/$lv1 lv_active "active"
vgchange -an $vg3
check lv_field $vg3/$lv1 lv_active ""

_clear_online_files

pvscan --cache -aay "$dev8"
check lv_field $vg3/$lv1 lv_active "active"
ls "$RUNDIR/lvm/pvs_online/$PVID8"
ls "$RUNDIR/lvm/vgs_online/$vg3"
vgchange -an $vg3

vgremove -ff $vg3

