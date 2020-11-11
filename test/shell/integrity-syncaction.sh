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

aux have_integrity 1 5 0 || skip
which mkfs.xfs || skip
which xfs_growfs || skip

mnt="mnt"
mkdir -p $mnt

aux prepare_devs 3 40

# Use awk instead of anoyingly long log out from printf
#printf "%0.sA" {1..16384} >> fileA
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 16384) printf "B" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

_prepare_vg() {
	# zero devs so we are sure to find the correct file data
	# on the underlying devs when corrupting it
	dd if=/dev/zero of="$dev1" bs=1M oflag=direct || true
	dd if=/dev/zero of="$dev2" bs=1M oflag=direct || true
	dd if=/dev/zero of="$dev3" bs=1M oflag=direct || true
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"
	pvs
}

_test1() {
	mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# we don't want fileA to be located too early in the fs,
	# otherwise activating the LV will trigger the corruption
	# to be found and corrected, leaving nothing for syncaction
	# to find and correct.
	dd if=/dev/urandom of=$mnt/rand16M bs=1M count=16

	cp fileA $mnt
	cp fileB $mnt
	cp fileC $mnt

	umount $mnt
	lvchange -an $vg/$lv1

	xxd "$dev1" > dev1.txt
	# corrupt fileB
	sed -e 's/4242 4242 4242 4242 4242 4242 4242 4242/4242 4242 4242 4242 4242 4242 4242 4243/' dev1.txt > dev1.bad
	rm -f dev1.txt
	xxd -r dev1.bad > "$dev1"
	rm -f dev1.bad

	lvchange -ay $vg/$lv1

	lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
	grep 0 mismatch

	lvchange --syncaction check $vg/$lv1

	_wait_recalc $vg/$lv1

	lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
	not grep 0 mismatch

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt
	cmp -b $mnt/fileA fileA
	cmp -b $mnt/fileB fileB
	cmp -b $mnt/fileC fileC
	umount $mnt
}

_test2() {
	mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# we don't want fileA to be located too early in the fs,
	# otherwise activating the LV will trigger the corruption
	# to be found and corrected, leaving nothing for syncaction
	# to find and correct.
	dd if=/dev/urandom of=$mnt/rand16M bs=1M count=16

	cp fileA $mnt
	cp fileB $mnt
	cp fileC $mnt

	umount $mnt
	lvchange -an $vg/$lv1

	# corrupt fileB and fileC on dev1
	xxd "$dev1" > dev1.txt
	sed -e 's/4242 4242 4242 4242 4242 4242 4242 4242/4242 4242 4242 4242 4242 4242 4242 4243/' dev1.txt > dev1.bad
	sed -e 's/4343 4343 4343 4343 4343 4343 4343 4343/4444 4444 4444 4444 4444 4444 4444 4444/' dev1.txt > dev1.bad
	rm -f dev1.txt
	xxd -r dev1.bad > "$dev1"
	rm -f dev1.bad

	# corrupt fileA on dev2
	xxd "$dev2" > dev2.txt
	sed -e 's/4141 4141 4141 4141 4141 4141 4141 4141/4141 4141 4141 4141 4141 4141 4145 4141/' dev2.txt > dev2.bad
	rm -f dev2.txt
	xxd -r dev2.bad > "$dev2"
	rm -f dev2.bad

	lvchange -ay $vg/$lv1

	lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
	grep 0 mismatch
	lvs -o integritymismatches $vg/${lv1}_rimage_1 |tee mismatch
	grep 0 mismatch

	lvchange --syncaction check $vg/$lv1

	_wait_recalc $vg/$lv1

	lvs -o integritymismatches $vg/${lv1}_rimage_0 |tee mismatch
	not grep 0 mismatch
	lvs -o integritymismatches $vg/${lv1}_rimage_1 |tee mismatch
	not grep 0 mismatch

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt
	cmp -b $mnt/fileA fileA
	cmp -b $mnt/fileB fileB
	cmp -b $mnt/fileC fileC
	umount $mnt
}

_sync_percent() {
	local checklv=$1
	get lv_field "$checklv" sync_percent | cut -d. -f1
}

_wait_recalc() {
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

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2"
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
_wait_recalc $vg/$lv1
_test1
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2"
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
_wait_recalc $vg/$lv1
_test2
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -l 6 $vg "$dev1" "$dev2" "$dev3"
_wait_recalc $vg/${lv1}_rimage_0
_wait_recalc $vg/${lv1}_rimage_1
_wait_recalc $vg/${lv1}_rimage_2
_wait_recalc $vg/$lv1
_test1
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

