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

. lib/inittest

which mkfs.xfs || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p $mnt

aux prepare_devs 5 64

# Use awk instead of anoyingly long log out from printf
#printf "%0.sA" {1..16384} >> fileA
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 16384) printf "B" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

# generate random data
dd if=/dev/urandom of=randA bs=512K count=2
dd if=/dev/urandom of=randB bs=512K count=3
dd if=/dev/urandom of=randC bs=512K count=4

_prepare_vg() {
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
	pvs
}

_add_new_data_to_mnt() {
        mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"

        mount "$DM_DEV_DIR/$vg/$lv1" $mnt

        # add original data
        cp randA $mnt
        cp randB $mnt
        cp randC $mnt
        mkdir $mnt/1
        cp fileA $mnt/1
        cp fileB $mnt/1
        cp fileC $mnt/1
        mkdir $mnt/2
        cp fileA $mnt/2
        cp fileB $mnt/2
        cp fileC $mnt/2
}

_add_more_data_to_mnt() {
        mkdir $mnt/more
        cp fileA $mnt/more
        cp fileB $mnt/more
        cp fileC $mnt/more
        cp randA $mnt/more
        cp randB $mnt/more
        cp randC $mnt/more
}

_verify_data_on_mnt() {
	diff randA $mnt/randA
	diff randB $mnt/randB
	diff randC $mnt/randC
	diff fileA $mnt/1/fileA
	diff fileB $mnt/1/fileB
	diff fileC $mnt/1/fileC
	diff fileA $mnt/2/fileA
	diff fileB $mnt/2/fileB
	diff fileC $mnt/2/fileC
}

_verify_data_on_lv() {
        lvchange -ay $vg/$lv1
        mount "$DM_DEV_DIR/$vg/$lv1" $mnt
        _verify_data_on_mnt
        rm $mnt/randA
        rm $mnt/randB
        rm $mnt/randC
        rm -rf $mnt/1
        rm -rf $mnt/2
        umount $mnt
        lvchange -an $vg/$lv1
}

_sync_percent() {
	local checklv=$1
	get lv_field "$checklv" sync_percent | cut -d. -f1
}

_wait_sync() {
	local checklv=$1

	for i in $(seq 1 10) ; do
		sync=$(_sync_percent "$checklv")
		echo "sync_percent is $sync"

		if test "$sync" = "100"; then
			return
		fi

		sleep 1
	done

	# TODO: There is some strange bug, first leg of RAID with integrity
	# enabled never gets in sync. I saw this in BB, but not when executing
	# the commands manually
	if test -z "$sync"; then
		echo "TEST\ WARNING: Resync of dm-integrity device '$checklv' failed"
                dmsetup status "$DM_DEV_DIR/mapper/${checklv/\//-}"
		exit
	fi
	echo "timeout waiting for recalc"
	return 1
}

# lvrename
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
_wait_sync $vg/${lv1}_rimage_0
_wait_sync $vg/${lv1}_rimage_1
_wait_sync $vg/$lv1
_add_new_data_to_mnt
umount $mnt
lvrename $vg/$lv1 $vg/$lv2
mount "$DM_DEV_DIR/$vg/$lv2" $mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv2
lvremove $vg/$lv2
vgremove -ff $vg

# lvconvert --replace
# an existing dev is replaced with another dev
# lv must be active
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
_wait_sync $vg/${lv1}_rimage_0
_wait_sync $vg/${lv1}_rimage_1
_wait_sync $vg/$lv1
lvs -o raidintegritymode $vg/$lv1 | grep journal
_add_new_data_to_mnt
lvconvert --replace "$dev1" $vg/$lv1 "$dev3"
lvs -a -o+devices $vg > out
cat out
grep "$dev2" out
grep "$dev3" out
not grep "$dev1" out
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# lvconvert --replace
# same as prev but with bitmap mode
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
_wait_sync $vg/${lv1}_rimage_0
_wait_sync $vg/${lv1}_rimage_1
_wait_sync $vg/$lv1
lvs -o raidintegritymode $vg/$lv1 | grep bitmap
_add_new_data_to_mnt
lvconvert --replace "$dev1" $vg/$lv1 "$dev3"
lvs -a -o+devices $vg > out
cat out
grep "$dev2" out
grep "$dev3" out
not grep "$dev1" out
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# lvconvert --repair
# while lv is active a device goes missing (with rimage,rmeta,imeta,orig).
# lvconvert --repair should replace the missing dev with another,
# (like lvconvert --replace does for a dev that's not missing).
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
_wait_sync $vg/${lv1}_rimage_0
_wait_sync $vg/${lv1}_rimage_1
_wait_sync $vg/$lv1
_add_new_data_to_mnt
aux disable_dev "$dev2"
lvs -a -o+devices $vg > out
cat out
grep unknown out
lvconvert -vvvv -y --repair $vg/$lv1
lvs -a -o+devices $vg > out
cat out
not grep "$dev2" out
not grep unknown out
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1
aux enable_dev "$dev2"
vgremove -ff $vg

# lvchange activationmode
# a device is missing (with rimage,rmeta,imeta,iorig), the lv
# is already inactive, and it cannot be activated, with
# activationmode degraded or partial, or in any way,
# until integrity is removed.

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
_wait_sync $vg/${lv1}_rimage_0
_wait_sync $vg/${lv1}_rimage_1
_wait_sync $vg/$lv1
_add_new_data_to_mnt
umount $mnt
lvchange -an $vg/$lv1
aux disable_dev "$dev2"
lvs -a -o+devices $vg
not lvchange -ay $vg/$lv1
not lvchange -ay --activationmode degraded $vg/$lv1
not lvchange -ay --activationmode partial $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvchange -ay --activationmode degraded $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt
_add_more_data_to_mnt
_verify_data_on_mnt
umount $mnt
lvchange -an $vg/$lv1
lvremove $vg/$lv1
aux enable_dev "$dev2"
vgremove -ff $vg

