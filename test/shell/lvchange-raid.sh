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

. lib/test

# Writemostly has been in every version since the begining
# Device refresh in 1.5.1 upstream and 1.3.4 < x < 1.4.0 in RHEL6
# Sync action    in 1.5.0 upstream and 1.3.3 < x < 1.4.0 in RHEL6
# Proper mismatch count 1.5.2 upstream,1.3.5 < x < 1.4.0 in RHEL6
#
# We will simplify and simple test for 1.5.2 and 1.3.5 < x < 1.4.0
aux target_at_least dm-raid 1 3 5 && 
  ! aux target_at_least dm-raid 1 4 0 ||
  aux target_at_least dm-raid 1 5 2 || skip

aux prepare_vg 6

# run_writemostly_check <VG> <LV>
run_writemostly_check() {
	local d0
	local d1

	printf "#\n#\n#\n# %s/%s (%s): run_writemostly_check\n#\n#\n#\n" \
		$1 $2 `lvs --noheadings -o segtype $1/$2`
	d0=`lvs -a --noheadings -o devices $1/${2}_rimage_0 | sed s/\(.\)//`
	d0=$(sed s/^[[:space:]]*// <<< "$d0")
	d1=`lvs -a --noheadings -o devices $1/${2}_rimage_1 | sed s/\(.\)//`
	d1=$(sed s/^[[:space:]]*// <<< "$d1")

	# No writemostly flag should be there yet.
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-.$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*-.$'

	if [ `lvs --noheadings -o segtype $1/$2` != "raid1" ]; then
		not lvchange --writemostly $d0 $1/$2
		return
	fi

	# Set the flag
	lvchange --writemostly $d0 $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w.$'

	# Running again should leave it set (not toggle)
	lvchange --writemostly $d0 $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w.$'

	# Running again with ':y' should leave it set
	lvchange --writemostly $d0:y $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w.$'

	# ':n' should unset it
	lvchange --writemostly $d0:n $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-.$'

	# ':n' again should leave it unset
	lvchange --writemostly $d0:n $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-.$'

	# ':t' toggle to set
	lvchange --writemostly $d0:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w.$'

	# ':t' toggle to unset
	lvchange --writemostly $d0:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-.$'

	# ':y' to set
	lvchange --writemostly $d0:y $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w.$'

	# Toggle both at once
	lvchange --writemostly $d0:t --writemostly $d1:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-.$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*w.$'

	# Toggle both at once again
	lvchange --writemostly $d0:t --writemostly $d1:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w.$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*-.$'

	# Toggle one, unset the other
	lvchange --writemostly $d0:n --writemostly $d1:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-.$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*w.$'

	# Toggle one, set the other
	lvchange --writemostly $d0:y --writemostly $d1:t $1/$2
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w.$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*-.$'

	# Partial flag supercedes writemostly flag
	aux disable_dev $d0
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*p.$'
	aux enable_dev $d0
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*w.$'

	# Catch Bad writebehind values
	not lvchange --writebehind "invalid" $1/$2
	not lvchange --writebehind -256 $1/$2

	# Set writebehind
	[ ! `lvs --noheadings -o raid_write_behind $1/$2` ]
	lvchange --writebehind 512 $1/$2
	[ `lvs --noheadings -o raid_write_behind $1/$2` -eq 512 ]

	# Converting to linear should clear flags and writebehind
	lvconvert -m 0 $1/$2 $d1
	lvconvert --type raid1 -m 1 $1/$2 $d1
	[ ! `lvs --noheadings -o raid_write_behind $1/$2` ]
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_0 | grep '.*-.$'
	lvs -a --noheadings -o lv_attr $1/${2}_rimage_1 | grep '.*-.$'
}

# run_syncaction_check <VG> <LV>
run_syncaction_check() {
	local device
	local seek
	local size
	local tmp

	printf "#\n#\n#\n# %s/%s (%s): run_syncaction_check\n#\n#\n#\n" \
		$1 $2 `lvs --noheadings -o segtype $1/$2`
	aux wait_for_sync $1 $2

	device=`lvs -a --noheadings -o devices $1/${2}_rimage_1 | sed s/\(.\)//`
	device=$(sed s/^[[:space:]]*// <<< "$device")

	size=`lvs -a --noheadings -o size --units 1k $1/${2}_rimage_1 | sed s/\.00k//`
	size=$(sed s/^[[:space:]]*// <<< "$size")
	size=$(($size / 2))

	tmp=`pvs --noheadings -o mda_size --units 1k $device | sed s/\.00k//`
	tmp=$(sed s/^[[:space:]]*// <<< "$tmp")
	seek=$tmp  # Jump over MDA

	tmp=`lvs -a --noheadings -o size --units 1k $1/${2}_rmeta_1 | sed s/\.00k//`
	tmp=$(sed s/^[[:space:]]*// <<< "$tmp")
	seek=$(($seek + $tmp))  # Jump over RAID metadata image

	seek=$(($seek + $size)) # Jump halfway through the RAID image

	lvs --noheadings -o lv_attr $1/$2 | grep '.*-.$'
	[ `lvs --noheadings -o raid_mismatch_count $1/$2` == 0 ]

	# Overwrite the last half of one of the PVs with crap
	dd if=/dev/urandom of=$device bs=1k count=$size seek=$seek

	# FIXME: Why is this necessary?  caching effects?
	# I don't need to do this when testing "real" devices...
	lvchange -an $1/$2; lvchange -ay $1/$2

	# "check" should find discrepancies but not change them
	# 'lvs' should show results
	lvchange --syncaction check $1/$2
	aux wait_for_sync $1 $2
	lvs --noheadings -o lv_attr $1/$2 | grep '.*m.$'
	[ `lvs --noheadings -o raid_mismatch_count $1/$2` != 0 ]

	# "repair" will fix discrepancies
	lvchange --syncaction repair $1/$2
	aux wait_for_sync $1 $2

	# Final "check" should show no mismatches
	# 'lvs' should show results
	lvchange --syncaction check $1/$2
	aux wait_for_sync $1 $2
	lvs --noheadings -o lv_attr $1/$2 | grep '.*-.$'
	[ `lvs --noheadings -o raid_mismatch_count $1/$2` == 0 ]
}

# run_refresh_check <VG> <LV>
#   Assumes "$dev2" is in the array
run_refresh_check() {
	local size

	printf "#\n#\n#\n# %s/%s (%s): run_refresh_check\n#\n#\n#\n" \
		$1 $2 `lvs --noheadings -o segtype $1/$2`

	aux wait_for_sync $1 $2

	size=`lvs -a --noheadings -o size --units 1k $1/$2 | sed s/\.00k//`
	size=$(sed s/^[[:space:]]*// <<< "$size")

	# Disable dev2 and do some I/O to make the kernel notice
	aux disable_dev "$dev2"
	dd if=/dev/urandom of=/dev/$1/$2 bs=1k count=$size

	# Check for 'p'artial flag
	lvs --noheadings -o lv_attr $1/$2 | grep '.*p.$'

	aux enable_dev "$dev2"

	# Check for 'r'efresh flag
	lvs --noheadings -o lv_attr $1/$2 | grep '.*r.$'

	lvchange --refresh $1/$2
	aux wait_for_sync $1 $2
	lvs --noheadings -o lv_attr $1/$2 | grep '.*-.$'

	# Writing random data above should mean that the devices
	# were out-of-sync.  The refresh should have taken care
	# of properly reintegrating the device.  If any mismatches
	# are repaired, it will show up in the 'lvs' output.
	lvchange --syncaction repair $1/$2
	aux wait_for_sync $1 $2
	lvs --noheadings -o lv_attr $1/$2 | grep '.*-.$'
}

# run_checks <VG> <LV> [snapshot_dev]
run_checks() {
	# Without snapshots
	run_writemostly_check $1 $2

	run_syncaction_check $1 $2

	run_refresh_check $1 $2

	# With snapshots
	if [ ! -z $3 ]; then
		lvcreate -s $1/$2 -l 4 -n snap $3

		run_writemostly_check $1 $2

		run_syncaction_check $1 $2

		run_refresh_check $1 $2

		lvremove -ff $1/snap
	fi
}

########################################################
# MAIN
########################################################

lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg "$dev1" "$dev2"
run_checks $vg $lv1 "$dev3"
lvremove -ff $vg

lvcreate --type raid4 -i 2 -l 4 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
run_checks $vg $lv1 "$dev5"
lvremove -ff $vg

lvcreate --type raid5 -i 2 -l 4 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
run_checks $vg $lv1 "$dev5"
lvremove -ff $vg

lvcreate --type raid6 -i 3 -l 6 -n $lv1 $vg \
		"$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
run_checks $vg $lv1 "$dev6"
lvremove -ff $vg

lvcreate --type raid10 -m 1 -i 2 -l 4 -n $lv1 $vg \
		"$dev1" "$dev2" "$dev3" "$dev4"
run_checks $vg $lv1 "$dev5"
lvremove -ff $vg
