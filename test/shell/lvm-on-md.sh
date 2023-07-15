#!/usr/bin/env bash

# Copyright (C) 2018-2021 Red Hat, Inc. All rights reserved.
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

. lib/inittest

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
PVS_ONLINE_DIR="$RUNDIR/lvm/pvs_online"
VGS_ONLINE_DIR="$RUNDIR/lvm/vgs_online"
HINTS="$RUNDIR/lvm/hints"

DFDIR="$LVM_SYSTEM_DIR/devices"
DF="$DFDIR/system.devices"

_clear_online_files() {
        # wait till udev is finished
        aux udev_wait
        rm -f "$PVS_ONLINE_DIR"/* "$VGS_ONLINE_DIR"/*
}


# This stops lvm from taking advantage of hints which
# will have already excluded md components.

# This stops lvm from asking udev if a dev is an md component.
# LVM will ask udev if a dev is an md component, but we don't
# want to rely on that ability in this test.
aux lvmconf "devices/md_component_detection = 1" \
	"devices/hints = \"none\"" \
	"devices/obtain_device_list_from_udev = 0" \
	"devices/search_for_devnames = \"none\""

aux extend_filter_md "a|/dev/md|"

aux prepare_devs 3

for level in 1 0 ; do

# create 2 disk MD raid1 array
# by default using metadata format 1.0 with data at the end of device
#
# When a raid0 md array is stopped, the components will not look like
# duplicate PVs as they do with raid1.
# mdadm does not seem to like --chunk=64 with raid1
case "$level" in
0) CHUNK="--chunk=64" ;;
*) CHUNK="" ;;
esac
aux mdadm_create --metadata=1.0 --level=$level $CHUNK --raid-devices=2 "$dev1" "$dev2"
mddev=$(< MD_DEV)

vgcreate $vg "$mddev"

lvmdevices || true
pvs -o+deviceidtype,deviceid

PVIDMD=$(get pv_field "$mddev" uuid | tr -d - )

lvcreate -n $lv1 -l 2 $vg
lvcreate -n $lv2 -l 2 -an $vg

lvchange -ay $vg/$lv2
check lv_field $vg/$lv1 lv_active "active"

# lvm does not show md components as PVs
pvs "$mddev"
not pvs "$dev1"
not pvs "$dev2"
pvs | tee out
not grep "$dev1" out
not grep "$dev2" out

vgchange -an $vg

# When the md device is started, lvm will see that and know to
# scan for md components, so stop the md device to remove this
# advantage so we will test the fallback detection.
mdadm --stop "$mddev"
aux udev_wait

# The md components should still be detected and excluded.
not pvs "$dev1"
not pvs "$dev2"
pvs | tee out
not grep "$dev1" out
not grep "$dev2" out

pvs 2>&1|tee out
not grep "Not using device" out

# should not activate from the md legs
not vgchange -ay $vg

# should not show an active lv
not dmsetup info $vg-$lv1

# should not allow updating vg
not lvcreate -l1 $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
not dmsetup info $vg-$lv1

aux mdadm_assemble "$mddev" "$dev1" "$dev2"

not pvs "$dev1"
not pvs "$dev2"
pvs | tee out
not grep "$dev1" out
not grep "$dev2" out

lvs $vg
vgchange -an $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
not dmsetup info $vg-$lv1

vgchange -ay $vg

check lv_field $vg/$lv1 lv_active "active"

vgchange -an $vg

_clear_online_files
pvscan --cache -aay "$mddev"

test -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test -f "$RUNDIR/lvm/vgs_online/$vg"

check active $vg $lv1

vgchange -an $vg
vgremove -f $vg

aux cleanup_md_dev
aux wipefs_a "$dev1" "$dev2"

done


# Repeat tests using the default config settings

aux lvmconf "devices/hints = \"all\"" \
	"devices/obtain_device_list_from_udev = 1" \
	"devices/search_for_devnames = \"none\""

rm $DF || true

# create 2 disk MD raid0 array
# by default using metadata format 1.0 with data at the end of device
# When a raid0 md array is stopped, the components will not look like
# duplicate PVs as they do with raid1.

aux mdadm_create --metadata=1.0 --level=0 --chunk=64 --raid-devices=2 "$dev1" "$dev2"
mddev=$(< MD_DEV)

# Create an unused PV so that there is at least one PV in the hints
# when the MD dev is stopped.  If there are no PVs, the hints are
# empty, and the code falls back to scanning all, and we do not end
# up testing the code with hints actively used.
pvcreate "$dev3"

vgcreate $vg "$mddev"

PVIDMD=$(get pv_field "$mddev" uuid | tr -d - )

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

grep "$mddev" "$HINTS"
grep "$dev3" "$HINTS"
not grep "$dev1" "$HINTS"
not grep "$dev2" "$HINTS"

vgchange -an $vg

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
grep -v -e WARNING -e "Devices file PVID" out1 > out2
not grep "Not using device" out2
not grep "$mddev" out2
not grep "$dev1" out2
not grep "$dev2" out2
grep "$dev3" out2
cat "$HINTS"

pvs 2>&1|tee out1
grep -v -e WARNING -e "Devices file PVID" out1 > out2
not grep "Not using device" out2
not grep "$mddev" out2
not grep "$dev1" out2
not grep "$dev2" out2
grep "$dev3" out2
cat "$HINTS"

# The md components should still be detected and excluded.
not pvs "$dev1"
not pvs "$dev2"
pvs | tee out
not grep "$dev1" out
not grep "$dev2" out
grep "$dev3" out

# should not activate from the md legs
not vgchange -ay $vg

# should not show an active lv
not dmsetup info $vg-$lv1

# should not allow updating vg
not lvcreate -l1 $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
not dmsetup info $vg-$lv1

# start the md dev
aux mdadm_assemble "$mddev" "$dev1" "$dev2"

not pvs "$dev1"
not pvs "$dev2"
pvs | tee out
not grep "$dev1" out
not grep "$dev2" out

lvs $vg
vgchange -an $vg

# should not activate from the md legs
_clear_online_files
pvscan --cache -aay "$dev1"
pvscan --cache -aay "$dev2"

test ! -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test ! -f "$RUNDIR/lvm/vgs_online/$vg"

# should not show an active lv
not dmsetup info $vg-$lv1

vgchange -ay $vg

check lv_field $vg/$lv1 lv_active "active"

vgchange -an $vg

_clear_online_files
pvscan --cache -aay "$mddev"

test -f "$RUNDIR/lvm/pvs_online/$PVIDMD"
test -f "$RUNDIR/lvm/vgs_online/$vg"

check active $vg $lv1

vgchange -an $vg
vgremove -f $vg

aux cleanup_md_dev
