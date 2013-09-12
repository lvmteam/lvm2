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

THIN_POSTFIX=""

# Writemostly has been in every version since the begining
# Device refresh in 1.5.1 upstream and 1.3.4 < x < 1.4.0 in RHEL6
# Sync action    in 1.5.0 upstream and 1.3.3 < x < 1.4.0 in RHEL6
# Proper mismatch count 1.5.2 upstream,1.3.5 < x < 1.4.0 in RHEL6
#
# We will simplify and simple test for 1.5.2 and 1.3.5 < x < 1.4.0
aux target_at_least dm-raid 1 3 5 && 
  ! aux target_at_least dm-raid 1 4 0 ||
  aux target_at_least dm-raid 1 5 2 || skip

# DEVICE "$dev6" is reserved for non-RAID LVs that
# will not undergo failure
aux prepare_vg 6

# run_writemostly_check <VG> <LV>
run_writemostly_check() {
	local d0
	local d1
	local vg=$1
	local lv=${2}${THIN_POSTFIX}

	printf "#\n#\n#\n# %s/%s (%s): run_writemostly_check\n#\n#\n#\n" \
		$vg $lv `lvs -a --noheadings -o segtype $vg/$lv`
	d0=`lvs -a --noheadings -o devices $vg/${lv}_rimage_0 | sed s/\(.\)//`
	d0=$(sed s/^[[:space:]]*// <<< "$d0")
	d1=`lvs -a --noheadings -o devices $vg/${lv}_rimage_1 | sed s/\(.\)//`
	d1=$(sed s/^[[:space:]]*// <<< "$d1")

	# No writemostly flag should be there yet.
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*-.$'
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_1 | grep '.*-.$'

	if [ `lvs -a --noheadings -o segtype $vg/$lv` != "raid1" ]; then
		not lvchange --writemostly $d0 $vg/$lv
		return
	fi

	# Set the flag
	lvchange --writemostly $d0 $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*w.$'

	# Running again should leave it set (not toggle)
	lvchange --writemostly $d0 $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*w.$'

	# Running again with ':y' should leave it set
	lvchange --writemostly $d0:y $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*w.$'

	# ':n' should unset it
	lvchange --writemostly $d0:n $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*-.$'

	# ':n' again should leave it unset
	lvchange --writemostly $d0:n $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*-.$'

	# ':t' toggle to set
	lvchange --writemostly $d0:t $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*w.$'

	# ':t' toggle to unset
	lvchange --writemostly $d0:t $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*-.$'

	# ':y' to set
	lvchange --writemostly $d0:y $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*w.$'

	# Toggle both at once
	lvchange --writemostly $d0:t --writemostly $d1:t $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*-.$'
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_1 | grep '.*w.$'

	# Toggle both at once again
	lvchange --writemostly $d0:t --writemostly $d1:t $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*w.$'
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_1 | grep '.*-.$'

	# Toggle one, unset the other
	lvchange --writemostly $d0:n --writemostly $d1:t $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*-.$'
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_1 | grep '.*w.$'

	# Toggle one, set the other
	lvchange --writemostly $d0:y --writemostly $d1:t $vg/$lv
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*w.$'
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_1 | grep '.*-.$'

	# Partial flag supercedes writemostly flag
	aux disable_dev $d0
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*p.$'

	# It is possible for the kernel to detect the failed device before
	# we re-enable it.  If so, the field will be set to 'r'efresh since
	# that also takes precedence over 'w'ritemostly.  If this has happened,
	# we refresh the LV and then check for 'w'.
	aux enable_dev $d0
	if lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*r.$'; then
		lvchange --refresh $vg/$lv
	fi
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*w.$'

	# Catch Bad writebehind values
	not lvchange --writebehind "invalid" $vg/$lv
	not lvchange --writebehind -256 $vg/$lv

	# Set writebehind
	[ ! `lvs --noheadings -o raid_write_behind $vg/$lv` ]
	lvchange --writebehind 512 $vg/$lv
	[ `lvs --noheadings -o raid_write_behind $vg/$lv` -eq 512 ]

	# Converting to linear should clear flags and writebehind
	lvconvert -m 0 $vg/$lv $d1
	lvconvert --type raid1 -m 1 $vg/$lv $d1
	[ ! `lvs --noheadings -o raid_write_behind $vg/$lv` ]
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_0 | grep '.*-.$'
	lvs -a --noheadings -o lv_attr $vg/${lv}_rimage_1 | grep '.*-.$'
}

# run_syncaction_check <VG> <LV>
run_syncaction_check() {
	local device
	local seek
	local size
	local tmp
	local vg=$1
	local lv=${2}${THIN_POSTFIX}

	printf "#\n#\n#\n# %s/%s (%s): run_syncaction_check\n#\n#\n#\n" \
		$vg $lv `lvs -a --noheadings -o segtype $vg/$lv`
	aux wait_for_sync $vg $lv

	device=`lvs -a --noheadings -o devices $vg/${lv}_rimage_1 | sed s/\(.\)//`
	device=$(sed s/^[[:space:]]*// <<< "$device")

	size=`lvs -a --noheadings -o size --units 1k $vg/${lv}_rimage_1 | sed s/\.00k//`
	size=$(sed s/^[[:space:]]*// <<< "$size")
	size=$(($size / 2))

	tmp=`pvs --noheadings -o mda_size --units 1k $device | sed s/\.00k//`
	tmp=$(sed s/^[[:space:]]*// <<< "$tmp")
	seek=$tmp  # Jump over MDA

	tmp=`lvs -a --noheadings -o size --units 1k $vg/${lv}_rmeta_1 | sed s/\.00k//`
	tmp=$(sed s/^[[:space:]]*// <<< "$tmp")
	seek=$(($seek + $tmp))  # Jump over RAID metadata image

	seek=$(($seek + $size)) # Jump halfway through the RAID image

	lvs --noheadings -o lv_attr $vg/$lv | grep '.*-.$'
	[ `lvs --noheadings -o raid_mismatch_count $vg/$lv` == 0 ]

	# Overwrite the last half of one of the PVs with crap
	dd if=/dev/urandom of=$device bs=1k count=$size seek=$seek

	if [ ! -z $THIN_POSTFIX ]; then
		#
		# Seems to work fine on real devices,
		# but can't make the system notice the bad blocks
		# in the testsuite - especially when thin is layered
		# on top of RAID.  In other cases, I can deactivate
		# and reactivate and it works.  Here, even that doesn't
		# work.
		return 0
		lvchange -an $vg/$2
		lvchange -ay $vg/$2
	else
		lvchange -an $vg/$lv
		lvchange -ay $vg/$lv
	fi

	# "check" should find discrepancies but not change them
	# 'lvs' should show results
	lvchange --syncaction check $vg/$lv
	aux wait_for_sync $vg $lv
	if ! lvs --noheadings -o lv_attr $vg/$lv | grep '.*m.$'; then
		lvs --noheadings -o lv_attr $vg/$lv
		dmsetup status | grep $vg
		false
	fi
	[ `lvs --noheadings -o raid_mismatch_count $vg/$lv` != 0 ]

	# "repair" will fix discrepancies
	lvchange --syncaction repair $vg/$lv
	aux wait_for_sync $vg $lv

	# Final "check" should show no mismatches
	# 'lvs' should show results
	lvchange --syncaction check $vg/$lv
	aux wait_for_sync $vg $lv
	lvs --noheadings -o lv_attr $vg/$lv | grep '.*-.$'
	[ `lvs --noheadings -o raid_mismatch_count $vg/$lv` == 0 ]
}

# run_refresh_check <VG> <LV>
#   Assumes "$dev2" is in the array
run_refresh_check() {
	local size
	local vg=$1
	local lv=${2}${THIN_POSTFIX}

	printf "#\n#\n#\n# %s/%s (%s): run_refresh_check\n#\n#\n#\n" \
		$vg $lv `lvs -a --noheadings -o segtype $vg/$lv`

	aux wait_for_sync $vg $lv

	if [ -z $THIN_POSTFIX ]; then
		size=`lvs -a --noheadings -o size --units 1k $vg/$lv | sed s/\.00k//`
	else
		size=`lvs -a --noheadings -o size --units 1k $vg/thinlv | sed s/\.00k//`
	fi
	size=$(sed s/^[[:space:]]*// <<< "$size")

	# Disable dev2 and do some I/O to make the kernel notice
	aux disable_dev "$dev2"
	if [ -z $THIN_POSTFIX ]; then
		dd if=/dev/urandom of=/dev/$vg/$lv bs=1k count=$size
	else
		dd if=/dev/urandom of=/dev/$vg/thinlv bs=1k count=$size
		sync; sync; sync
	fi

	# Check for 'p'artial flag
	lvs --noheadings -o lv_attr $vg/$lv | grep '.*p.$'
	dmsetup status
	lvs -a -o name,attr,devices $vg

	aux enable_dev "$dev2"

	dmsetup status
	lvs -a -o name,attr,devices $vg

	# Check for 'r'efresh flag
	lvs --noheadings -o lv_attr $vg/$lv | grep '.*r.$'

	lvchange --refresh $vg/$lv
	aux wait_for_sync $vg $lv
	lvs --noheadings -o lv_attr $vg/$lv | grep '.*-.$'

	# Writing random data above should mean that the devices
	# were out-of-sync.  The refresh should have taken care
	# of properly reintegrating the device.
	lvchange --syncaction repair $vg/$lv
	aux wait_for_sync $vg $lv
	lvs --noheadings -o lv_attr $vg/$lv | grep '.*-.$'
}

# run_recovery_rate_check <VG> <LV>
#   Assumes "$dev2" is in the array
run_recovery_rate_check() {
	local vg=$1
	local lv=${2}${THIN_POSTFIX}

	printf "#\n#\n#\n# %s/%s (%s): run_recovery_rate_check\n#\n#\n#\n" \
		$vg $lv `lvs -a --noheadings -o segtype $vg/$lv`

	lvchange --minrecoveryrate 50 $vg/$lv
	lvchange --maxrecoveryrate 100 $vg/$lv

	[ `lvs --noheadings -o raid_min_recovery_rate $vg/$lv` == "50" ]
	[ `lvs --noheadings -o raid_max_recovery_rate $vg/$lv` == "100" ]
}

# run_checks <VG> <LV> <"-"|snapshot_dev|"thinpool_data"|"thinpool_meta">
run_checks() {
	THIN_POSTFIX=""

	if [ -z $3 ]; then
		printf "#\n#\n# run_checks: Too few arguments\n#\n#\n"
		return 1
	elif [ '-' == $3 ]; then
		printf "#\n#\n# run_checks: Simple check\n#\n#\n"

		run_writemostly_check $1 $2
		run_syncaction_check $1 $2
		run_refresh_check $1 $2
		run_recovery_rate_check $1 $2
	elif [ 'thinpool_data' == $3 ]; then
		aux target_at_least dm-thin-pool 1 8 0 || return 0

		# RAID works EX in cluster
		# thinpool works EX in cluster
		# but they don't work together in a cluster yet
		#  (nor does thinpool+mirror work in a cluster yet)
		test -e LOCAL_CLVMD && return 0
		printf "#\n#\n# run_checks: RAID as thinpool data\n#\n#\n"

# Hey, specifying devices for thin allocation doesn't work
#		lvconvert --thinpool $1/$2 "$dev6"
		lvcreate -aey -L 2M -n ${2}_meta $1 "$dev6"
		lvconvert --thinpool $1/$2 --poolmetadata ${2}_meta
		lvcreate -T $1/$2 -V 1 -n thinlv
		THIN_POSTFIX="_tdata"

		run_writemostly_check $1 $2
		run_syncaction_check $1 $2
		run_refresh_check $1 $2
		run_recovery_rate_check $1 $2
	elif [ 'thinpool_meta' == $3 ]; then
		aux target_at_least dm-thin-pool 1 8 0 || return 0
		test -e LOCAL_CLVMD && return 0
		printf "#\n#\n# run_checks: RAID as thinpool metadata\n#\n#\n"

		lvrename $1/$2 ${2}_meta
		lvcreate -aey -L 2M -n $2 $1 "$dev6"
		lvconvert --thinpool $1/$2 --poolmetadata ${2}_meta
		lvcreate -T $1/$2 -V 1 -n thinlv
		THIN_POSTFIX="_tmeta"

		run_writemostly_check $1 $2
		run_syncaction_check $1 $2
		run_refresh_check $1 $2
		run_recovery_rate_check $1 $2
	elif [ 'snapshot' == $3 ]; then
		printf "#\n#\n# run_checks: RAID under snapshot\n#\n#\n"
		lvcreate -aey -s $1/$2 -l 4 -n snap "$dev6"

		run_writemostly_check $1 $2
		run_syncaction_check $1 $2
		run_refresh_check $1 $2
		run_recovery_rate_check $1 $2

		lvremove -ff $1/snap
	else
		printf "#\n#\n# run_checks: Invalid argument\n#\n#\n"
		return 1
	fi
}

########################################################
# MAIN
########################################################

for i in "-" "snapshot" "thinpool_data" "thinpool_meta"; do
	lvcreate --type raid1 -m 1 -L 2M -n $lv1 $vg \
			"$dev1" "$dev2"
	run_checks $vg $lv1 $i
	lvremove -ff $vg

	lvcreate --type raid4 -i 2 -L 2M -n $lv1 $vg \
			"$dev1" "$dev2" "$dev3" "$dev4"
	run_checks $vg $lv1 $i
	lvremove -ff $vg

	lvcreate --type raid5 -i 2 -L 2M -n $lv1 $vg \
			"$dev1" "$dev2" "$dev3" "$dev4"
	run_checks $vg $lv1 $i
	lvremove -ff $vg

	lvcreate --type raid6 -i 3 -L 2M -n $lv1 $vg \
			"$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
	run_checks $vg $lv1 $i
	lvremove -ff $vg

	lvcreate --type raid10 -m 1 -i 2 -L 2M -n $lv1 $vg \
			"$dev1" "$dev2" "$dev3" "$dev4"
	run_checks $vg $lv1 $i
	lvremove -ff $vg
done
