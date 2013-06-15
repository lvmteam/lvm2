#!/bin/sh
# Copyright (C) 2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# This test ensures that 'lvchange --refresh vg/raid_lv' properly restores
# a transiently failed device in RAID LVs.

. lib/test

# dm-raid v1.4.1+ contains RAID10 support
aux target_at_least dm-raid 1 4 1 || skip

aux prepare_vg 5

# run_writemostly_check <VG> <LV>
run_writemostly_check() {
	d0=`lvs -a --noheadings -o devices $1/${2}_rimage_0 | sed s/\(.\)//`
	d0=$(sed s/^[[:space:]]*// <<< "$d0")
	d1=`lvs -a --noheadings -o devices $1/${2}_rimage_1 | sed s/\(.\)//`
	d1=$(sed s/^[[:space:]]*// <<< "$d1")

	# No writemostly flag should be there yet.
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*-$'

	if [ `lvs --noheadings -o segtype $1/$2` != "raid1" ]; then
		not lvchange --writemostly $d0 $1/$2
		return
	fi

	# Set the flag
	lvchange --writemostly $d0 $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w$'

	# Running again should leave it set (not toggle)
	lvchange --writemostly $d0 $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w$'

	# Running again with ':y' should leave it set
	lvchange --writemostly $d0:y $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w$'

	# ':n' should unset it
	lvchange --writemostly $d0:n $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-$'

	# ':n' again should leave it unset
	lvchange --writemostly $d0:n $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-$'

	# ':t' toggle to set
	lvchange --writemostly $d0:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w$'

	# ':t' toggle to unset
	lvchange --writemostly $d0:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-$'

	# ':y' to set
	lvchange --writemostly $d0:y $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w$'

	# Toggle both at once
	lvchange --writemostly $d0:t --writemostly $d1:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*w$'

	# Toggle both at once again
	lvchange --writemostly $d0:t --writemostly $d1:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*-$'

	# Toggle one, unset the other
	lvchange --writemostly $d0:n --writemostly $d1:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*w$'

	# Toggle one, set the other
	lvchange --writemostly $d0:y --writemostly $d1:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*-$'

	# Partial flag supercedes writemostly flag
	aux disable_dev $d0
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*p$'
	aux enable_dev $d0
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w$'

	# Catch Bad writebehind values
	not lvchange --writebehind "invalid" $1/$2
	not lvchange --writebehind -256 $1/$2

	# Set writebehind
	[ ! `lvs --noheadings -o writebehind $1/$2` ]
	lvchange --writebehind 512 $1/$2
	[ `lvs --noheadings -o writebehind $1/$2` -eq 512 ]

	# Converting to linear should clear flags and writebehind
	lvconvert -m 0 $1/$2 $d1
	lvconvert --type raid1 -m 1 $1/$2 $d1
	[ ! `lvs --noheadings -o writebehind $1/$2` ]
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*-$'
}

# run_syncaction_check <VG> <LV>
run_syncaction_check() {
	local device
	local seek
	local size

	aux wait_for_sync $1 $2

	device=`lvs -a --noheadings -o devices $1/${2}_rimage_1 | sed s/\(.\)//`
	device=$(sed s/^[[:space:]]*// <<< "$device")
	size=`lvs -a --noheadings -o size --units 1k $1/$2 | sed s/\.00k//`
	size=$(sed s/^[[:space:]]*// <<< "$size")
	size=$(($size / 2))
	seek=`pvs --noheadings -o mda_size --units 1k $device | sed s/\.00k//`
	seek=$(sed s/^[[:space:]]*// <<< "$seek")
	seek=$(($size + $seek))

	# Check all is normal
	if ! lvs --noheadings -o lv_attr $1/$2 | grep '.*-$' ||
		[ `lvs --noheadings -o mismatches $1/$2` != 0 ]; then
		#
		# I think this is a kernel bug.  It happens randomly after
		# a RAID device creation.  I think the mismatch count
		# should not be set unless a check or repair is run.
		#
		echo "Strange... RAID has mismatch count after creation."

		# Run "check" should turn up clean
		lvchange --syncaction check $1/$2
		aux wait_for_sync $1 $2
		sync
	fi

	lvs --noheadings -o lv_attr $1/$2 | grep '.*-$'
	[ `lvs --noheadings -o mismatches $1/$2` == 0 ]

	# Overwrite the last half of one of the PVs with crap
	dd if=/dev/urandom of=$device bs=1k count=$size seek=$seek

	# FIXME: Why is this necessary?  caching effects?
	# I don't need to do this when testing "real" devices...
	lvchange -an $1/$2; lvchange -ay $1/$2

	# "check" should find discrepancies but not change them
	# 'lvs' should show results
	lvchange --syncaction check $1/$2
	aux wait_for_sync $1 $2
	lvs --noheadings -o lv_attr $1/$2 | grep '.*m$'
	[ `lvs --noheadings -o mismatches $1/$2` != 0 ]

	# "repair" will fix discrepancies and record number fixed
	lvchange --syncaction repair $1/$2
	aux wait_for_sync $1 $2
	lvs --noheadings -o lv_attr $1/$2 | grep '.*m$'
	[ `lvs --noheadings -o mismatches $1/$2` != 0 ]

	# Final "check" should show no mismatches
	# 'lvs' should show results
	lvchange --syncaction check $1/$2
	aux wait_for_sync $1 $2
	lvs --noheadings -o lv_attr $1/$2 | grep '.*-$'
	[ `lvs --noheadings -o mismatches $1/$2` == 0 ]
}

# run_refresh_check <VG> <LV>
#   Assumes "$dev2" is in the array
run_refresh_check() {
	aux wait_for_sync $1 $2

	# Disable dev2 and do some I/O to make the kernel notice
	aux disable_dev "$dev2"
	dd if=/dev/urandom of=/dev/$1/$2 bs=4M count=1

	# Check for 'p'artial flag
	lvs --noheadings -o lv_attr $1/$2 | grep '.*p$'

	aux enable_dev "$dev2"

	# Check for 'r'efresh flag
	lvs --noheadings -o lv_attr $1/$2 | grep '.*r$'

	lvchange --refresh $1/$2

	# Writing random data above should mean that the devices
	# were out-of-sync.  The refresh should have taken care
	# of properly reintegrating the device.  If any mismatches
	# are repaired, it will show up in the 'lvs' output.
	lvchange --syncaction repair $1/$2
	aux wait_for_sync $1 $2
	lvs --noheadings -o lv_attr $1/$2 | grep '.*-$'
}

run_checks() {
	if aux target_at_least dm-raid 1 1 0; then
		run_writemostly_check $1 $2
	fi

	if aux target_at_least dm-raid 1 5 0; then
		run_syncaction_check $1 $2
	fi

	if aux target_at_least dm-raid 1 5 1; then
		run_refresh_check $1 $2
	fi
}

########################################################
# MAIN
########################################################

lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg "$dev1" "$dev2"
run_checks $vg $lv1
lvremove -ff $vg

lvcreate --type raid4 -i 2 -l 4 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
run_checks $vg $lv1
lvremove -ff $vg

lvcreate --type raid5 -i 2 -l 4 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
run_checks $vg $lv1
lvremove -ff $vg

lvcreate --type raid6 -i 3 -l 6 -n $lv1 $vg \
		"$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
run_checks $vg $lv1
lvremove -ff $vg

lvcreate --type raid10 -m 1 -i 2 -l 4 -n $lv1 $vg \
		"$dev1" "$dev2" "$dev3" "$dev4"
run_checks $vg $lv1
lvremove -ff $vg
