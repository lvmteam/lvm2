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

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
PVS_ONLINE_DIR="$RUNDIR/lvm/pvs_online"
VGS_ONLINE_DIR="$RUNDIR/lvm/vgs_online"

_clear_online_files() {
	# wait till udev is finished
	aux udev_wait
	rm -f "$PVS_ONLINE_DIR"/*
	rm -f "$VGS_ONLINE_DIR"/*
}

. lib/inittest

aux prepare_pvs 2

vgcreate $vg1 "$dev1" "$dev2"
vgs | grep $vg1

pvscan --cache

vgs | grep $vg1

# Check that an LV cannot be activated by lvchange while VG is exported
lvcreate -n $lv1 -l 4 -a n $vg1
check lv_exists $vg1
vgexport $vg1
fail lvs $vg1
fail lvchange -ay $vg1/$lv1
vgimport $vg1
check lv_exists $vg1
check lv_field $vg1/$lv1 lv_active ""

# Check that an LV cannot be activated by pvscan while VG is exported
vgchange -an $vg1
_clear_online_files
vgexport $vg1
pvscan --cache -aay "$dev1" || true
pvscan --cache -aay "$dev2" || true
_clear_online_files
vgimport $vg1
check lv_exists $vg1
check lv_field $vg1/$lv1 lv_active ""
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"
check lv_field $vg1/$lv1 lv_active "active"
lvchange -an -vvvv $vg1/$lv1
dmsetup ls
lvremove $vg1/$lv1

# When MDA is ignored on PV, do not read any VG
# metadata from such PV as it may contain old
# metadata which hasn't been updated for some
# time and also since the MDA is marked as ignored,
# it should really be *ignored*!
_clear_online_files
pvchange --metadataignore y "$dev1"
aux disable_dev "$dev2"
pvscan --cache
check pv_field "$dev1" vg_name "[unknown]"
aux enable_dev "$dev2"
pvscan --cache
check pv_field "$dev1" vg_name "$vg1"

vgremove -ff $vg1
