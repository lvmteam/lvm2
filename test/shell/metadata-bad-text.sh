#!/usr/bin/env bash

# Copyright (C) 2008-2013,2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

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

aux prepare_devs 3
get_devs

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

pvs

dd if="$dev1" of=meta1 bs=4k count=2

sed 's/flags =/flagx =/' meta1 > meta1.bad

dd if=meta1.bad of="$dev1"

pvs 2>&1 | tee out
grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# bad metadata in one mda doesn't prevent using
# the VG since other mdas are fine and usable
lvcreate -l1 $vg


vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

vgchange -an $vg
vgremove -ff $vg


#
# Same test as above, but corrupt metadata text
# on two of the three PVs, leaving one good
# copy of the metadata.
#

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

pvs

dd if="$dev1" of=meta1 bs=4k count=2
dd if="$dev2" of=meta2 bs=4k count=2

sed 's/READ/RRRR/' meta1 > meta1.bad
sed 's/seqno =/sss =/' meta2 > meta2.bad

dd if=meta1.bad of="$dev1"
dd if=meta2.bad of="$dev2"

pvs 2>&1 | tee out
grep "bad metadata text" out > out2
grep "$dev1" out2
grep "$dev2" out2

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# bad metadata in one mda doesn't prevent using
# the VG since other mdas are fine
lvcreate -l1 $vg


vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

vgchange -an $vg
vgremove -ff $vg

#
# Three PVs where two have one mda, and the third
# has two mdas.  The first mda is corrupted on all
# thee PVs, but the second mda on the third PV
# makes the VG usable.
#

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

pvcreate "$dev1"
pvcreate "$dev2"
pvcreate --pvmetadatacopies 2 "$dev3"

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

pvs

dd if="$dev1" of=meta1 bs=4k count=2
dd if="$dev2" of=meta2 bs=4k count=2
dd if="$dev3" of=meta3 bs=4k count=2

sed 's/READ/RRRR/' meta1 > meta1.bad
sed 's/seqno =/sss =/' meta2 > meta2.bad
sed 's/id =/id/' meta3 > meta3.bad

dd if=meta1.bad of="$dev1"
dd if=meta2.bad of="$dev2"
dd if=meta3.bad of="$dev3"

pvs 2>&1 | tee out
grep "bad metadata text" out > out2
grep "$dev1" out2
grep "$dev2" out2
grep "$dev3" out2

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# bad metadata in some mdas doesn't prevent using
# the VG if there's a good mda found
lvcreate -l1 $vg


vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

vgchange -an $vg
vgremove -ff $vg

#
# Test that vgck --updatemetadata will update old metadata
# and repair bad metadata text at the same time from different
# devices.
#

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

pvcreate "$dev1"
pvcreate "$dev2"
pvcreate --pvmetadatacopies 2 "$dev3"

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

# Put bad metadata onto dev1
dd if="$dev1" of=meta1 bs=4k count=2
sed 's/READ/RRRR/' meta1 > meta1.bad
dd if=meta1.bad of="$dev1"

pvs 2>&1 | tee out
grep "bad metadata text" out > out2
grep "$dev1" out2

# We can still use the VG with other available
# mdas, skipping the bad mda.

lvcreate -n $lv1 -l1 -an $vg "$dev1"
lvcreate -n $lv2 -l1 -an $vg "$dev1"

# Put old metadata onto dev2 by updating
# the VG while dev2 is disabled.
aux disable_dev "$dev2"

pvs
pvs "$dev1"
not pvs "$dev2"
pvs "$dev3"
lvs $vg/$lv1
lvs $vg/$lv2

lvremove $vg/$lv2

aux enable_dev "$dev2"

# Both old and bad metadata are reported.
pvs 2>&1 | tee out
grep "ignoring metadata seqno" out
grep "bad metadata text" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

# fixes the bad metadata on dev1, and
# fixes the old metadata on dev2.
vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "ignoring metadata seqno" out
not grep "bad metadata text" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1

vgchange -an $vg
vgremove -ff $vg

#
# Test pvscan activation with bad PVs
#

# autoactivation not done on shared VGs
if test -n "$LVM_TEST_LVMLOCKD"; then
exit 0
fi

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

PVID1=`pvs $dev1 --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVID1
PVID2=`pvs $dev2 --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVID2
PVID3=`pvs $dev3 --noheading -o uuid | tr -d - | awk '{print $1}'`
echo $PVID3

pvs

dd if="$dev1" of=meta1 bs=4k count=2
dd if="$dev2" of=meta2 bs=4k count=2

sed 's/READ/RRRR/' meta1 > meta1.bad
sed 's/seqno =/sss =/' meta2 > meta2.bad

dd if=meta1.bad of="$dev1"
dd if=meta2.bad of="$dev2"

pvs 2>&1 | tee out
grep "bad metadata text" out > out2
grep "$dev1" out2
grep "$dev2" out2

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# bad metadata in one mda doesn't prevent using
# the VG since other mdas are fine
lvcreate -l1 -n $lv1 $vg

vgchange -an $vg

_clear_online_files

# pvscan of one dev with bad metadata will result
# in the dev acting like a PV without metadata,
# which causes pvscan to scan all devs to find the
# VG it belongs to.  In this case it finds all the
# other devs in the VG are online and activates the
# VG.
pvscan --cache -aay "$dev1"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
ls "$RUNDIR/lvm/vgs_online/$vg"

lvs $vg
check lv_field $vg/$lv1 lv_active "active"
vgchange -an $vg

_clear_online_files

# scan the one pv with good metadata, does not scan any others
pvscan --cache -aay "$dev3"
not ls "$RUNDIR/lvm/pvs_online/$PVID1"
not ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
not ls "$RUNDIR/lvm/vgs_online/$vg"

# scan the next pv with bad metadata, causes pvscan to scan
# and find all PVs and activate
pvscan --cache -aay "$dev2"
ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
ls "$RUNDIR/lvm/vgs_online/$vg"

check lv_field $vg/$lv1 lv_active "active"
vgchange -an $vg


vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

vgchange -an $vg
vgremove -ff $vg

