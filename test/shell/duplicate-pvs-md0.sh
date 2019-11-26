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

test -f /proc/mdstat && grep -q raid0 /proc/mdstat || \
        modprobe raid0 || skip

mddev="/dev/md33"
not grep $mddev /proc/mdstat || skip

aux lvmconf 'devices/md_component_detection = 1'

# This stops lvm from asking udev if a dev is an md component.
# LVM will ask udev if a dev is an md component, but we don't
# want to rely on that ability in this test.
aux lvmconf 'devices/obtain_device_list_from_udev = 0'

aux extend_filter_md "a|/dev/md|"

aux prepare_devs 4

# Create an unused PV so that there is at least one PV in the hints
# when the MD dev is stopped.  If there are no PVs, the hints are
# empty, and the code falls back to scanning all, and we do not end
# up testing the code with hints actively used.
pvcreate "$dev3"

## Test variations:
# PV on md raid1|raid0, md_component_checks auto|start, mddev start|stopped,
# one raid dev disabled when mddev is stopped.


##########################################
# PV on an md raid0 device, auto+started
# md_component_checks: auto (not start)
# mddev: started            (not stopped)
#

aux lvmconf 'devices/md_component_checks = "auto"'

mdadm --create --metadata=1.0 "$mddev" --level 0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"
pvcreate "$mddev"
PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

# pvs shows only the md dev as PV
pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs > out
grep $mddev out
not grep "$dev1" out
not grep "$dev2" out
grep "$mddev" $HINTS
not grep "$dev1" $HINTS
not grep "$dev2" $HINTS

# normal activation works
lvchange -ay $vg/$lv1
lvs -o active $vg |tee out || true
grep "active" out
vgchange -an $vg

# pvscan activation all works
_clear_online_files
pvscan --cache -aay
ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
ls "$RUNDIR/lvm/vgs_online/$vg"
lvs -o active $vg |tee out || true
grep "active" out
vgchange -an $vg

# pvscan activation from mddev works
_clear_online_files
pvscan --cache -aay "$mddev"
ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
ls "$RUNDIR/lvm/vgs_online/$vg"
lvs -o active $vg |tee out || true
grep "active" out
vgchange -an $vg

# pvscan activation from md components does nothing
_clear_online_files
pvscan --cache -aay "$dev1"
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"
pvscan --cache -aay "$dev2"
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"
lvs -o active $vg |tee out || true
not grep "active" out

vgchange -an $vg
vgremove -f $vg
mdadm --stop "$mddev"
aux udev_wait
aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait


##########################################
# PV on an md raid0 device, start+started
# md_component_checks: start (not auto)
# mddev: started             (not stopped)
#

aux lvmconf 'devices/md_component_checks = "start"'

mdadm --create --metadata=1.0 "$mddev" --level 0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"
pvcreate "$mddev"
PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

# pvs shows only the md dev as PV
pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs > out
grep $mddev out
not grep "$dev1" out
not grep "$dev2" out
# N.B. in this case hints are disabled for duplicate pvs seen by scan
# it would be preferrable if this didn't happen as in auto mode, but it's ok.
# grep "$mddev" $HINTS
not grep "$dev1" $HINTS
not grep "$dev2" $HINTS

# normal activation works
lvchange -ay $vg/$lv1
lvs -o active $vg |tee out || true
grep "active" out
vgchange -an $vg

# pvscan activation all works
_clear_online_files
pvscan --cache -aay
ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
ls "$RUNDIR/lvm/vgs_online/$vg"
lvs -o active $vg |tee out || true
grep "active" out
vgchange -an $vg

# pvscan activation from mddev works
_clear_online_files
pvscan --cache -aay "$mddev"
ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
ls "$RUNDIR/lvm/vgs_online/$vg"
lvs -o active $vg |tee out || true
grep "active" out
vgchange -an $vg

# N.B. with raid0 the component because the PV/size difference
# triggers and md component check, whereas with raid1 it doesn't.
_clear_online_files
pvscan --cache -aay "$dev1"
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"
lvs -o active $vg |tee out || true
not grep "active" out

vgchange -an $vg
vgremove -f $vg
mdadm --stop "$mddev"
aux udev_wait
aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait


##########################################
# PV on an md raid0 device, auto+stopped
# md_component_checks: auto (not start)
# mddev: stopped            (not started)
#

aux lvmconf 'devices/md_component_checks = "auto"'

mdadm --create --metadata=1.0 "$mddev" --level 0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"
pvcreate "$mddev"
PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

mdadm --stop "$mddev"
cat /proc/mdstat

# pvs does not show the PV
not pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep $mddev out
not grep "$dev1" out
not grep "$dev2" out
pvscan --cache
not grep "$mddev" $HINTS
not grep "$dev1" $HINTS
not grep "$dev2" $HINTS

# the vg is not seen, normal activation does nothing
not lvchange -ay $vg/$lv1
not lvs $vg

# pvscan activation all does nothing
_clear_online_files
pvscan --cache -aay
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# pvscan activation from md components does nothing
_clear_online_files
pvscan --cache -aay "$dev1"
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"
pvscan --cache -aay "$dev2"
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait

##########################################
# PV on an md raid0 device, start+stopped
# md_component_checks: start (not auto)
# mddev: stopped             (not started)
#

aux lvmconf 'devices/md_component_checks = "start"'

mdadm --create --metadata=1.0 "$mddev" --level 0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"
pvcreate "$mddev"
PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

mdadm --stop "$mddev"
cat /proc/mdstat

# pvs does not show the PV
not pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep $mddev out
not grep "$dev1" out
not grep "$dev2" out
pvscan --cache
not grep "$mddev" $HINTS
# N.B. it would be preferrable if dev1 did not appear in hints but it's ok
# not grep "$dev1" $HINTS
not grep "$dev2" $HINTS

# the vg is not seen, normal activation does nothing
not lvchange -ay $vg/$lv1
not lvs $vg

# pvscan activation all does nothing
_clear_online_files
pvscan --cache -aay
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# pvscan activation from md components does nothing
_clear_online_files
pvscan --cache -aay "$dev1" || true
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"
pvscan --cache -aay "$dev2" || true
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"
lvs -o active $vg |tee out || true
not grep "active" out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait


##########################################
# PV on an md raid0 device, start+stopped
# md_component_checks: start (not auto)
# mddev: stopped             (not started)
# only one raid dev online
#

aux lvmconf 'devices/md_component_checks = "start"'

mdadm --create --metadata=1.0 "$mddev" --level 0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"
pvcreate "$mddev"
PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

mdadm --stop "$mddev"
cat /proc/mdstat

# disable one dev of the md device
aux disable_dev "$dev2"

# pvs does not show the PV
not pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep $mddev out
not grep "$dev1" out
not grep "$dev2" out
pvscan --cache
not grep "$mddev" $HINTS
# N.B. would be preferrable for this md component to not be in hints
# grep "$dev1" $HINTS
not grep "$dev2" $HINTS

not lvchange -ay $vg/$lv1
not lvs $vg

_clear_online_files
pvscan --cache -aay
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

_clear_online_files
pvscan --cache -aay "$dev1"
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

aux enable_dev "$dev2"
aux aux udev_wait
cat /proc/mdstat
# for some reason enabling dev2 starts an odd md dev
mdadm --stop $(lsblk -al -o NAME --noheadings "$dev2" | grep '^md') || true
cat /proc/mdstat
aux wipefs_a "$dev1" || true
aux wipefs_a "$dev2" || true
aux udev_wait

##########################################
# PV on an md raid0 device, auto+stopped
# md_component_checks: auto (not start)
# mddev: stopped            (not started)
# only one raid dev online
#

aux lvmconf 'devices/md_component_checks = "auto"'

mdadm --create --metadata=1.0 "$mddev" --level 0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
aux wait_md_create "$mddev"
pvcreate "$mddev"
PVIDMD=`pvs $mddev --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVIDMD
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

mdadm --stop "$mddev"
cat /proc/mdstat

# disable one leg
aux disable_dev "$dev2"

# pvs does not show the PV
not pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs > out
not grep $mddev out
not grep "$dev1" out
not grep "$dev2" out
pvscan --cache
not grep "$mddev" $HINTS
not grep "$dev1" $HINTS
not grep "$dev2" $HINTS

# the vg is not seen, normal activation does nothing
not lvchange -ay $vg/$lv1
not lvs $vg

# pvscan activation all does nothing
_clear_online_files
pvscan --cache -aay
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# pvscan activation from md components does nothing
_clear_online_files
pvscan --cache -aay "$dev1"
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"
pvscan --cache -aay "$dev2"
not ls "$RUNDIR/lvm/pvs_online/$PVIDMD"
not ls "$RUNDIR/lvm/vgs_online/$vg"

aux enable_dev "$dev2"
aux aux udev_wait
cat /proc/mdstat
# for some reason enabling dev2 starts an odd md dev
mdadm --stop $(lsblk -al -o NAME --noheadings "$dev2" | grep '^md') || true
cat /proc/mdstat
aux wipefs_a "$dev1" || true
aux wipefs_a "$dev2" || true
aux udev_wait
