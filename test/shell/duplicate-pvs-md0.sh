#!/usr/bin/env bash

# Copyright (C) 2012-2021 Red Hat, Inc. All rights reserved.
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
        rm -f "$PVS_ONLINE_DIR"/* "$VGS_ONLINE_DIR"/*
}

. lib/inittest

MD_LEVEL=${MD_LEVEL-0}

aux prepare_devs 4 10

# Create an unused PV so that there is at least one PV in the hints
# when the MD dev is stopped.  If there are no PVs, the hints are
# empty, and the code falls back to scanning all, and we do not end
# up testing the code with hints actively used.
pvcreate "$dev3"

## Test variations:
# PV on md raid1|raid0, md_component_checks auto|start, mddev start|stopped,
# one raid dev disabled when mddev is stopped.

# LVM will ask udev if a dev is an md component, but we don't
# want to rely on that ability in this test so stops lvm from
# asking udev if a dev is an md component.
aux lvmconf "devices/obtain_device_list_from_udev = 0" \
	    "devices/md_component_detection = 1" \
	    "devices/md_component_checks = \"auto\""

aux extend_filter_md "a|/dev/md|"

# Run in 2 passes  - 1st. with "auto"    2nd. with "start" component checks
for pass in "auto" "start" ; do

##########################################
# PV on an md raidX device
# md_component_checks: auto|start  (not start)
# mddev: started                   (not stopped)
#
aux mdadm_create --metadata=1.0 --level="$MD_LEVEL" --chunk=64 --raid-devices=2 "$dev1" "$dev2"
mddev=$(< MD_DEV)
lvmdevices --adddev $mddev || true

pvcreate "$mddev"
PVIDMD=$(get pv_field "$mddev" uuid | tr -d - )
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

# pvs shows only the md dev as PV
pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs | tee out
grep "$mddev" out
not grep "$dev1" out
not grep "$dev2" out
# N.B. in this case hints are disabled for duplicate pvs seen by scan
# it would be preferrable if this didn't happen as in auto mode, but it's ok.
test "$pass" = "auto" && grep "$mddev" "$HINTS"
not grep "$dev1" "$HINTS"
not grep "$dev2" "$HINTS"

# normal activation works
lvchange -ay $vg/$lv1
check active $vg $lv1
vgchange -an $vg

# pvscan activation all works
_clear_online_files
pvscan --cache -aay
test -e "$RUNDIR/lvm/pvs_online/$PVIDMD"
test -e "$RUNDIR/lvm/vgs_online/$vg"
check active $vg $lv1
vgchange -an $vg

# pvscan activation from mddev works
_clear_online_files
pvscan --cache -aay "$mddev"
test -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test -f "$RUNDIR/lvm/vgs_online/$vg"
check active $vg $lv1
vgchange -an $vg

# pvscan activation from md components does nothing
_clear_online_files
pvscan --cache -aay "$dev1"
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

if [ "$pass" = "auto" ] ; then
	pvscan --cache -aay "$dev2"
	test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
	test ! -f "$RUNDIR/lvm/vgs_online/$vg"
fi
# N.B. with raid0 the component because the PV/size difference
# triggers and md component check, whereas with raid1 it doesn't.
check inactive $vg $lv1

vgchange -an $vg
vgremove -f $vg
lvmdevices --deldev $mddev || true
aux cleanup_md_dev


##########################################
# PV on an md raidX device
# md_component_checks: auto|start
# mddev: stopped       (not started)
#

aux mdadm_create --metadata=1.0 --level="$MD_LEVEL" --chunk=64 --raid-devices=2 "$dev1" "$dev2"
mddev=$(< MD_DEV)
lvmdevices --adddev $mddev || true

pvcreate "$mddev"
PVIDMD=$(get pv_field "$mddev" uuid | tr -d - )
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

mdadm --stop "$mddev"
cat /proc/mdstat

# pvs does not show the PV
not pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs | tee out
not grep "$mddev" out
# N.B. it would be preferrable if dev1 did not appear in hints but it's ok
# not grep "$dev1" $HINTS
not grep "$dev1" out
not grep "$dev2" out
pvscan --cache
not grep "$mddev" "$HINTS"
not grep "$dev1" "$HINTS"
not grep "$dev2" "$HINTS"

# the vg is not seen, normal activation does nothing
not lvchange -ay $vg/$lv1
not lvs $vg

# pvscan activation all does nothing
_clear_online_files
pvscan --cache -aay
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

# pvscan activation from md components does nothing
_clear_online_files
pvscan --cache -aay "$dev1"
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"
pvscan --cache -aay "$dev2"
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

#lvs -o active $vg |tee out || true
#not grep "active" out

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait

##########################################
# PV on an md raidX device
# md_component_checks: auto|start
# mddev: stopped       (not started)
# only one raid dev online
#

aux mdadm_create --metadata=1.0 --level="$MD_LEVEL" --chunk=64 --raid-devices=2 "$dev1" "$dev2"
mddev=$(< MD_DEV)
lvmdevices --adddev $mddev || true

pvcreate "$mddev"
PVIDMD=$(get pv_field "$mddev" uuid | tr -d - )
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
pvs | tee out
not grep "$mddev" out
not grep "$dev1" out
not grep "$dev2" out
pvscan --cache
not grep "$mddev" "$HINTS"
# N.B. would be preferrable for this md component to not be in hints
# grep "$dev1" $HINTS
not grep "$dev1" "$HINTS"
not grep "$dev2" "$HINTS"

# the vg is not seen, normal activation does nothing
not lvchange -ay $vg/$lv1
not lvs $vg

# pvscan activation all does nothing
_clear_online_files
pvscan --cache -aay
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

# pvscan activation from md components does nothing
_clear_online_files
pvscan --cache -aay "$dev1"
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"
pvscan --cache -aay "$dev2"
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

aux enable_dev "$dev2"
lvmdevices --deldev $mddev || true
aux cleanup_md_dev

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux udev_wait

if [ "$MD_LEVEL" = "1" ] ; then
##########################################
# PV on an md raid1 device, auto+stopped
# md_component_checks: auto|start
# mddev: stopped       (not started)
# three raid images
#
aux mdadm_create --metadata=1.0 --level="$MD_LEVEL" --chunk=64 --raid-devices=3 "$dev1" "$dev2" "$dev4"
mddev=$(< MD_DEV)
lvmdevices --adddev $mddev || true

pvcreate "$mddev"
PVIDMD=$(get pv_field "$mddev" uuid | tr -d - )
vgcreate $vg "$mddev"
lvcreate -n $lv1 -l 2 -an $vg

mdadm --stop "$mddev"
cat /proc/mdstat

# pvs does not show the PV
not pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
not pvs "$dev4"
pvs | tee out
not grep "$mddev" out
not grep "$dev1" out
not grep "$dev2" out
not grep "$dev4" out
pvscan --cache
not grep "$mddev" "$HINTS"
not grep "$dev1" "$HINTS"
not grep "$dev2" "$HINTS"
not grep "$dev4" "$HINTS"

# the vg is not seen, normal activation does nothing
not lvchange -ay $vg/$lv1
not lvs $vg

# pvscan activation all does nothing
_clear_online_files
pvscan --cache -aay
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

# pvscan activation from md components does nothing
_clear_online_files
pvscan --cache -aay "$dev1"
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"
pvscan --cache -aay "$dev2"
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"
pvscan --cache -aay "$dev4"
test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

aux wipefs_a "$dev1"
aux wipefs_a "$dev2"
aux wipefs_a "$dev4"
aux udev_wait
fi   # MD_LEVEL == 1

# next loop with 'start'
test "$pass" != "auto" || aux lvmconf "devices/md_component_checks = \"start\""

done
