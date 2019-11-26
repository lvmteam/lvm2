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

/* hints are currently disabled with lvmlockd */
SKIP_WITH_LVMLOCKD=1

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
HINTS="$RUNDIR/lvm/hints"
NOHINTS="$RUNDIR/lvm/nohints"
NEWHINTS="$RUNDIR/lvm/newhints"
PREV="$RUNDIR/lvm/prev-hints"

. lib/inittest

# TODO:
# Test commands that ignore hints
# Test flock


aux lvmconf 'devices/scan_lvs = 0'

aux prepare_devs 6

# no PVs yet so hints should have no devs
pvs
not grep scan: $HINTS

#
# vg1 uses dev1,dev2
#
# Test basics that PVs are in hints, not non-PV devs,
# and that only PVs are scanned when using hints.
#

vgcreate $vg1 "$dev1" "$dev2"
lvcreate -n $lv1 -l 4 $vg1

# test that only the two PVs are in hints
pvs
grep -v -E "$dev1|$dev2" $HINTS > tmptest
not grep scan: tmptest

# test that 'pvs' submits only three reads, one for each PV in hints
# for initial scan, and one more in vg_read rescan check

if [ -e "/usr/bin/strace" ]; then
strace -e io_submit pvs 2>&1|tee tmptest
test "$(grep io_submit tmptest | wc -l)" -eq 3

# test that 'pvs -a' submits seven reads, one for each device,
# and one more in vg_read rescan check
strace -e io_submit pvs -a 2>&1|tee tmptest
test "$(grep io_submit tmptest | wc -l)" -eq 7
fi

#
# vg2 uses dev3,dev4
#
# Test common commands that cause hints to be refreshed:
# pvcreate/vgcreate/vgextend/vgreduce/vgremove/pvremove
#

not pvs "$dev3"
not grep "$dev3" $HINTS
cp $HINTS $PREV
pvcreate "$dev3"
grep "# Created empty" $HINTS
cat $NEWHINTS
# next cmd recreates hints
pvs "$dev3"
grep "$dev3" $HINTS
not diff $HINTS $PREV
not cat $NEWHINTS

not vgs $vg2
cp $HINTS $PREV
vgcreate $vg2 "$dev3"
grep "# Created empty" $HINTS
cat $NEWHINTS
# next cmd recreates hints
vgs $vg2
grep $vg2 $HINTS
not diff $HINTS $PREV
not cat $NEWHINTS

cp $HINTS $PREV
vgextend $vg2 "$dev4"
grep "# Created empty" $HINTS
cat $NEWHINTS
# next cmd recreates hints
vgs $vg2
grep "$dev4" $HINTS
not diff $HINTS $PREV
not cat $NEWHINTS

cp $HINTS $PREV
vgreduce $vg2 "$dev4"
grep "# Created empty" $HINTS
cat $NEWHINTS
# next cmd recreates hints
vgs $vg2
grep "$dev4" $HINTS
not diff $HINTS $PREV
not cat $NEWHINTS

cp $HINTS $PREV
vgremove $vg2
grep "# Created empty" $HINTS
cat $NEWHINTS
# next cmd recreates hints
not vgs $vg2
not grep $vg2 $HINTS
not diff $HINTS $PREV
not cat $NEWHINTS

cp $HINTS $PREV
pvremove "$dev3" "$dev4"
grep "# Created empty" $HINTS
cat $NEWHINTS
# next cmd recreates hints
not pvs "$dev3"
not pvs "$dev4"
not grep "$dev3" $HINTS
not grep "$dev4" $HINTS
not diff $HINTS $PREV
not cat $NEWHINTS

#
# Test that adding a new device and removing a device
# causes hints to be recreated.
#

not pvs "$dev5"

# create a new temp device that will cause hint hash to change
DEVNAME=${PREFIX}pv99
echo "0 `blockdev --getsize $dev5` linear $dev5 0" | dmsetup create $DEVNAME
dmsetup status $DEVNAME

cp $HINTS $PREV
# pvs ignores current hints because of different dev hash and refreshes new hints
pvs
# devs listed in hints before and after are the same
grep scan: $PREV > scan1
grep scan: $HINTS > scan2
diff scan1 scan2
# hash listed before and after are different
cat $PREV
cat $HINTS
grep devs_hash $PREV > devs_hash1
grep devs_hash $HINTS > devs_hash2
not diff devs_hash1 devs_hash2

# hints are stable/unchanging
cp $HINTS $PREV
pvs
diff $HINTS $PREV

# remove the temp device which will cause hint hash to change again
dmsetup remove $DEVNAME

cp $HINTS $PREV
# pvs ignores current hints because of different dev hash and refreshes new hints
pvs
# devs listed in hints before and after are the same
grep scan: $PREV > scan1
grep scan: $HINTS > scan2
diff scan1 scan2
# hash listed before and after are different
grep devs_hash $PREV > devs_hash1
grep devs_hash $HINTS > devs_hash2
not diff devs_hash1 devs_hash2

#
# Test that hints don't change from a bunch of commands
# that use hints and shouldn't change it.
#

# first create some more metadata using vg2
pvcreate "$dev3" "$dev4"
vgcreate $vg2 "$dev3"
lvcreate -n $lv1 -l1 $vg2
lvcreate -n $lv2 -l1 $vg2

cp $HINTS $PREV
lvm fullreport
lvchange -ay $vg1
lvchange -an $vg1
lvcreate -l1 -n $lv2 $vg1
lvcreate -l1 -an -n $lv3 $vg1
lvchange -an $vg1
lvremove $vg1/$lv3
lvresize -l+1 $vg1/$lv2
lvresize -l-1 $vg1/$lv2
lvdisplay
pvdisplay
vgdisplay
lvs
pvs
vgs
vgchange -ay $vg2
vgchange -an $vg2
vgck $vg2
lvrename $vg1 $lv2 $lv3
# no change in hints after all that
diff $HINTS $PREV

#
# Test that changing the filter will cause hint refresh
#

rm $HINTS $PREV
vgs
cp $HINTS $PREV
# this changes the filter to exclude dev5 which is not a PV
aux hide_dev "$dev5"
# next cmd sees different filter, ignores hints, creates new hints
pvs
not diff $HINTS $PREV
# run cmds using new filter
pvs
cp $HINTS $PREV
vgs
# hints are stable once refreshed
diff $HINTS $PREV
# this changes the filter to include dev5
aux unhide_dev "$dev5"
# next cmd sees different filter, ignores hints, creates new hints
pvs
not diff $HINTS $PREV
# hints are stable
cp $HINTS $PREV
vgs
diff $HINTS $PREV

#
# Test that changing scan_lvs will cause hint refresh
# 

rm $HINTS $PREV
vgs
cp $HINTS $PREV
# change lvm.conf
aux lvmconf 'devices/scan_lvs = 1'
# next cmd sees new setting, ignores hints, creates new hints
pvs
not diff $HINTS $PREV
# run cmds using new filter
pvs
cp $HINTS $PREV
vgs
# hints are stable once refreshed
diff $HINTS $PREV
# change lvm.conf back
aux lvmconf 'devices/scan_lvs = 0'
# next cmd sees different scan_lvs, ignores hints, creates new hints
pvs
not diff $HINTS $PREV
# hints are stable once refreshed
cp $HINTS $PREV
pvs
diff $HINTS $PREV

#
# Test pvscan --cache to force hints refresh
#

# pvs (no change), pvscan (hints are new), pvs (no change)
rm $HINTS $PREV
pvs
cp $HINTS $PREV
# this next pvscan recreates the hints file
pvscan --cache
# the only diff will be "Created by pvscan ..." vs "Created by pvs ..."
not diff $HINTS $PREV
cp $HINTS $PREV
pvs
diff $HINTS $PREV
grep 'Created by pvscan' $HINTS
# dev4 is a PV not used by a VG, dev5 is not a PV
# using dd to copy skirts hint tracking so dev5 won't be seen
# (unless the dd triggers udev which triggers pvscan --cache $dev5,
# but I've not seen that happen in tests so far.)
dd if="$dev4" of="$dev5" bs=1M
# this pvs won't see dev5
pvs > foo
cat foo
grep "$dev4" foo
not grep "$dev5" foo
# no hints have changed after dd and pvs since dd cannot be detected
diff $HINTS $PREV
# force hints refresh, will see duplicate now
pvscan --cache
not diff $HINTS $PREV
cat $HINTS
pvs -a > foo
# after force refresh, both devs (dups) appear in output
cat foo
grep "$dev4" foo
grep "$dev5" foo
# clear PV from dev5
dd if=/dev/zero of="$dev5" bs=1M count=1
# this pvs won't use hints because of duplicate PVs,
# and will create new hints
cp $HINTS $PREV
pvs > foo
not diff $HINTS $PREV
grep "$dev4" foo
not grep "$dev5" foo
grep "$dev4" $HINTS
not grep "$dev5" $HINTS

#
# Test pvscan --cache <dev> forces refresh
#

rm $HINTS $PREV
pvs
cp $HINTS $PREV
# this next pvscan creates newhints to trigger a refresh
pvscan --cache "$dev5"
cat $NEWHINTS
# this next pvs creates new hints
pvs
# the only diff will be "Created by..."
not diff $HINTS $PREV



#
# Test incorrect dev-to-pvid info in hints is detected
# dev4 is a PV not in a VG
#

pvs
cp $HINTS tmp-old
# this pvchange will invalidate current hints
pvchange -u "$dev4"
grep "# Created empty" $HINTS
cat $NEWHINTS
# this next pvs will create new hints with the new uuid
pvs
grep "$dev4" $HINTS > tmp-newuuid
cp $HINTS tmp-new
not diff tmp-old tmp-new
# hints are stable
pvs
diff $HINTS tmp-new
# replace the current hints with the old hints with the old uuid
cp tmp-old $HINTS
# this next pvs will see wrong dev-to-pvid mapping and invalidate hints
pvs
cat $HINTS
cat $NEWHINTS
# this next pvs will create new hints with the new uuid
pvs
cat $HINTS
grep -f tmp-newuuid $HINTS
rm tmp-old tmp-new tmp-newuuid


#
# Test incorrent pvid-to-vgname info in hints is detected
#

# this vgcreate invalidates current hints
vgcreate $vg3 $dev4
# this pvs creates new hints
pvs
cp $HINTS tmp-old
# this vgrename will invalidate current hints
vgrename $vg3 $vg4
# this pvs will create new hints with the new vg name
pvs
cp $HINTS tmp-new
not diff tmp-old tmp-new
# replace the current hints with the old hints with the old vg name
cp tmp-old $HINTS
# this pvs will see wrong pvid-to-vgname mapping and invalidate hints
pvs
cat $NEWHINTS
# this pvs will create new hints with the new vg name
pvs
grep $vg4 $HINTS

vgremove -y $vg4
vgremove -y $vg2
vgremove -y $vg1

