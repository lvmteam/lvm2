#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
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

_clear_online_files() {
        # wait till udev is finished
        aux udev_wait
        rm -f "$PVS_ONLINE_DIR"/*
        rm -f "$VGS_ONLINE_DIR"/*
}

. lib/inittest

test -f /proc/mdstat && grep -q raid1 /proc/mdstat || \
	modprobe raid1 || skip

aux lvmconf 'devices/md_component_detection = 1'

# This stops lvm from taking advantage of hints which
# will have already excluded md components.
aux lvmconf 'devices/hints = "none"'

# This stops lvm from asking udev if a dev is an md component.
# LVM will ask udev if a dev is an md component, but we don't
# want to rely on that ability in this test.
aux lvmconf 'devices/obtain_device_list_from_udev = 0'

aux extend_filter_LVMTEST "a|/dev/md|"

aux prepare_devs 2

# create 2 disk MD raid1 array
# by default using metadata format 1.0 with data at the end of device
aux prepare_md_dev 1 64 2 "$dev1" "$dev2"

cat /proc/mdstat

mddev=$(< MD_DEV)
pvdev=$(< MD_DEV_PV)

vgcreate $vg "$mddev"

lvcreate -n $lv1 -l 2 $vg
lvcreate -n $lv2 -l 2 -an $vg

lvchange -ay $vg/$lv2
check lv_field $vg/$lv1 lv_active "active"

# lvm does not show md components as PVs
pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out

sleep 1

vgchange -an $vg
sleep 1

# When the md device is started, lvm will see that and know to
# scan for md components, so stop the md device to remove this
# advantage so we will test the fallback detection.
mdadm --stop "$mddev"
aux udev_wait

# The md components should still be detected and excluded.
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out

pvs -vvvv

# should not activate from the md legs
not vgchange -ay $vg

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

# should not allow updating vg
not lvcreate -l1 $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

# start the md dev
mdadm --assemble "$mddev" "$dev1" "$dev2"
aux udev_wait

# Now that the md dev is online, pvs can see it
# and check for components even if
# md_component_checks is "start" (which disables
# most default end-of-device scans)
aux lvmconf 'devices/md_component_checks = "start"'

not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out


vgchange -ay $vg

check lv_field $vg/$lv1 lv_active "active"

vgchange -an $vg
aux udev_wait

vgremove -f $vg

aux cleanup_md_dev

# Put this setting back to the default
aux lvmconf 'devices/md_component_checks = "auto"'

# create 2 disk MD raid0 array
# by default using metadata format 1.0 with data at the end of device
# When a raid0 md array is stopped, the components will not look like
# duplicate PVs as they do with raid1.
aux prepare_md_dev 0 64 2 "$dev1" "$dev2"

cat /proc/mdstat

mddev=$(< MD_DEV)
pvdev=$(< MD_DEV_PV)

vgcreate $vg "$mddev"

lvs $vg

lvcreate -n $lv1 -l 2 $vg
lvcreate -n $lv2 -l 2 -an $vg

lvchange -ay $vg/$lv2
check lv_field $vg/$lv1 lv_active "active"

# lvm does not show md components as PVs
pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out

sleep 1

vgchange -an $vg
sleep 1

# When the md device is started, lvm will see that and know to
# scan for md components, so stop the md device to remove this
# advantage so we will test the fallback detection.
mdadm --stop "$mddev"
aux udev_wait

# The md components should still be detected and excluded.
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out

pvs -vvvv

# should not activate from the md legs
not vgchange -ay $vg

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

# should not allow updating vg
not lvcreate -l1 $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

# start the md dev
mdadm --assemble "$mddev" "$dev1" "$dev2"
aux udev_wait

# Now that the md dev is online, pvs can see it
# and check for components even if
# md_component_checks is "start" (which disables
# most default end-of-device scans)
aux lvmconf 'devices/md_component_checks = "start"'

not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out

vgchange -ay $vg 2>&1 |tee out

check lv_field $vg/$lv1 lv_active "active"

vgchange -an $vg
aux udev_wait

vgremove -f $vg

aux cleanup_md_dev

