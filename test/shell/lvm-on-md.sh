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
SKIP_WITH_LVMLOCKD=1

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
PVS_ONLINE_DIR="$RUNDIR/lvm/pvs_online"
VGS_ONLINE_DIR="$RUNDIR/lvm/vgs_online"
HINTS="$RUNDIR/lvm/hints"

_clear_online_files() {
        # wait till udev is finished
        aux udev_wait
        rm -f "$PVS_ONLINE_DIR"/*
        rm -f "$VGS_ONLINE_DIR"/*
}

. lib/inittest

test -f /proc/mdstat && grep -q raid1 /proc/mdstat || \
	modprobe raid1 || skip

mddev="/dev/md33"
not grep $mddev /proc/mdstat || skip

aux lvmconf 'devices/md_component_detection = 1'

# This stops lvm from taking advantage of hints which
# will have already excluded md components.
aux lvmconf 'devices/hints = "none"'

# This stops lvm from asking udev if a dev is an md component.
# LVM will ask udev if a dev is an md component, but we don't
# want to rely on that ability in this test.
aux lvmconf 'devices/obtain_device_list_from_udev = 0'

aux extend_filter_md "a|/dev/md|"

aux prepare_devs 3

# create 2 disk MD raid1 array
# by default using metadata format 1.0 with data at the end of device

mdadm --create --metadata=1.0 "$mddev" --level 1 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"
vgcreate $vg "$mddev"

PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD

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

pvs 2>&1|tee out
not grep "Not using device" out

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

not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

mdadm --assemble "$mddev" "$dev1" "$dev2"
aux udev_wait

not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out

lvs $vg
vgchange -an $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

vgchange -ay $vg

check lv_field $vg/$lv1 lv_active "active"

vgchange -an $vg

_clear_online_files
pvscan --cache -aay "$mddev"

ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
ls "$RUNDIR/lvm/vgs_online/$vg"

lvs -o active $vg |tee out || true
grep "active" out

vgchange -an $vg

aux udev_wait

vgremove -f $vg

mdadm --stop "$mddev"
aux udev_wait
aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait


# create 2 disk MD raid0 array
# by default using metadata format 1.0 with data at the end of device
# When a raid0 md array is stopped, the components will not look like
# duplicate PVs as they do with raid1.

mdadm --create --metadata=1.0 "$mddev" --level 0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"
vgcreate $vg "$mddev"

PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD

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

pvs 2>&1|tee out
not grep "Not using device" out

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

not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

# start the md dev
mdadm --assemble "$mddev" "$dev1" "$dev2"
aux udev_wait

not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out

lvs $vg
vgchange -an $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

vgchange -ay $vg

check lv_field $vg/$lv1 lv_active "active"

vgchange -an $vg

_clear_online_files
pvscan --cache -aay "$mddev"

ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
ls "$RUNDIR/lvm/vgs_online/$vg"

lvs -o active $vg |tee out || true
grep "active" out

vgchange -an $vg

aux udev_wait

vgremove -f $vg

mdadm --stop "$mddev"
aux udev_wait
aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait


# Repeat tests using the default config settings

aux lvmconf 'devices/hints = "all"'
aux lvmconf 'devices/obtain_device_list_from_udev = 1'

# create 2 disk MD raid0 array
# by default using metadata format 1.0 with data at the end of device
# When a raid0 md array is stopped, the components will not look like
# duplicate PVs as they do with raid1.

mdadm --create --metadata=1.0 "$mddev" --level 0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"

# Create an unused PV so that there is at least one PV in the hints
# when the MD dev is stopped.  If there are no PVs, the hints are
# empty, and the code falls back to scanning all, and we do not end
# up testing the code with hints actively used.
pvcreate "$dev3"

vgcreate $vg "$mddev"

PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD

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

grep "$mddev" $HINTS
grep "$dev3" $HINTS
not grep "$dev1" $HINTS
not grep "$dev2" $HINTS

sleep 1

vgchange -an $vg
sleep 1

# When the md device is started, lvm will see that and know to
# scan for md components, so stop the md device to remove this
# advantage so we will test the fallback detection.
mdadm --stop "$mddev"
aux udev_wait

# A WARNING indicating duplicate PVs is printed by 'pvs' in this
# case.  It's printed during the scan, but after the scan, the
# md component detection is run on the devs and they are dropped
# when we see they are md components. So, we ignore the warning
# containing the word duplicate, and look for the "Not using device"
# message, which shouldn't appear, as it would indicate that
# we didn't drop the md components.
# FIXME: we should avoid printing the premature warning indicating
# duplicate PVs which are eventually recognized as md components
# and dropped.
pvs 2>&1|tee out1
grep -v WARNING out1 > out2
not grep "Not using device" out2
not grep "$mddev" out2
not grep "$dev1" out2
not grep "$dev2" out2
grep "$dev3" out2
cat $HINTS

pvs 2>&1|tee out1
grep -v WARNING out1 > out2
not grep "Not using device" out2
not grep "$mddev" out2
not grep "$dev1" out2
not grep "$dev2" out2
grep "$dev3" out2
cat $HINTS

# The md components should still be detected and excluded.
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out
grep "$dev3" out

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

not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

# start the md dev
mdadm --assemble "$mddev" "$dev1" "$dev2"
aux udev_wait

not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep "$dev1" out
not grep "$dev2" out

lvs $vg
vgchange -an $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
rm out
lvs -o active $vg |tee out || true
not grep "active" out

vgchange -ay $vg

check lv_field $vg/$lv1 lv_active "active"

vgchange -an $vg

_clear_online_files
pvscan --cache -aay "$mddev"

ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
ls "$RUNDIR/lvm/vgs_online/$vg"

lvs -o active $vg |tee out || true
grep "active" out

vgchange -an $vg

aux udev_wait

vgremove -f $vg

mdadm --stop "$mddev"
aux udev_wait
aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait

