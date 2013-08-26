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

test_description="ensure pvmove works with all segment types"

. lib/test

which mkfs.ext2 || skip
which md5sum || skip

# ---------------------------------------------------------------------
# Utilities
lvdev_() {
	echo "$DM_DEV_DIR/$1/$2"
}

# lv_is_on_pvs <VG> <LV> <PV1> <PV2> ...
lv_is_on_pvs() {
	local vg=$1
	local lv_list=( $2 )
	local new_lv_list=()
	local run_again=true

	shift 2

	while $run_again; do
		run_again=false
		for tmplv in ${lv_list[*]}; do
			if [[ $(lvs -a --noheadings -o attr $vg/$tmplv) =~ ^[[:space:]]*r.* ]] ||
			   [[ $(lvs -a --noheadings -o attr $vg/$tmplv) =~ ^[[:space:]]*R.* ]]; then
				# Expand RAID
				echo "$tmplv is a RAID LV"
				new_lv_list=( ${new_lv_list[*]} `lvs -a --noheadings -o name | grep ${tmplv}_r` )
				run_again=true
			elif [[ $(lvs -a --noheadings -o attr $vg/$tmplv) =~ ^[[:space:]]*m.* ]] ||
			     [[ $(lvs -a --noheadings -o attr $vg/$tmplv) =~ ^[[:space:]]*M.* ]]; then
				# Expand Mirror
				echo "$tmplv is a mirror LV"
				new_lv_list=( ${new_lv_list[*]} `lvs -a --noheadings -o devices $vg/$tmplv | sed s/,/' '/g` )
				new_lv_list=( ${new_lv_list[*]} `lvs -a --noheadings -o mirror_log $vg/$tmplv | sed s/,/' '/g` )
				run_again=true
			elif [[ $(lvs -a --noheadings -o attr $vg/$tmplv) =~ ^[[:space:]]*t.* ]]; then
				# Expand Thin Pool
				echo "$tmplv is a thin-pool LV"
				new_lv_list=( ${new_lv_list[*]} `lvs -a --noheadings -o data_lv,metadata_lv $vg/$tmplv | sed s/,/' '/g` )
				run_again=true
			elif [[ $(lvs -a --noheadings -o attr $vg/$tmplv) =~ ^[[:space:]]*V.* ]]; then
				# Expand Thin-LV
				echo "$tmplv is a thin-LV"
				new_lv_list=( ${new_lv_list[*]} `lvs -a --noheadings -o pool_lv $vg/$tmplv | sed s/,/' '/g` )
				run_again=true
			elif [[ $(lvs -a --noheadings -o attr $vg/$tmplv) =~ ^[[:space:]]*e.* ]] &&
			     lvs -a --noheadings -o devices $vg/$tmplv | grep $tmplv; then
				# Expand Thin Pool Meta, which is also RAID
				echo "$tmplv is a thinpool (RAID) metadata"
				new_lv_list=( ${new_lv_list[*]} `lvs -a --noheadings -o name | grep ${tmplv}_r` )
				run_again=true
			else
				new_lv_list=( ${new_lv_list[*]} $tmplv )
			fi
		done
		printf "\nOld LV list: ${lv_list[*]}\n\n"

		lv_list=()
		for tmplv in ${new_lv_list[*]}; do
			lv_list=( ${lv_list[*]} $(echo $tmplv | sed s/\(.*\)/''/ | sed s/\\[/''/ | sed s/]/''/) )
		done
		printf "\nExpanded LV list: ${lv_list[*]}\n\n"

		new_lv_list=()
	done

	[ -e out-$$ ] && rm out-$$
	for tmplv in ${lv_list[*]}; do
		lvs -a -odevices --noheadings $vg/$tmplv | sed 's/,/\n/g' >> out-$$
	done

	#is on all specified devs
	for d in $*; do
		if ! grep "$d(" out-$$; then
			cat out-$$
			false
		fi
	done

	#isn't on any other dev (we are set -e remember)
	cat out-$$
	[ -e tmp-$$ ] && rm tmp-$$
	for d in $*; do
		echo "Removing $d"
		! grep -v "$d(" out-$$ >> tmp-$$
		mv tmp-$$ out-$$
		cat out-$$
	done
	# out-$$ must be empty - no additional devs allowed
	[ ! -s out-$$ ]

	rm out-$$
	return 0
}

save_dev_sum_() {
  mkfs.ext2 $1 > /dev/null && md5sum $1 > md5.$(basename $1)
}

check_dev_sum_() {
  md5sum -c md5.$(basename $1)
}
# ---------------------------------------------------------------------

aux prepare_vg 5 30

# Each of the following tests does:
# 1) Create two LVs - one linear and one other segment type
#    The two LVs will share a PV.
# 2) Move both LVs together
# 3) Move only the second LV by name

printf "##\n# Testing pvmove of linear LV\n##\n"
lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -l 2 -n $lv1 $vg "$dev1"
lv_is_on_pvs $vg ${lv1}_foo "$dev1"
lv_is_on_pvs $vg $lv1 "$dev1"
save_dev_sum_ $(lvdev_ $vg $lv1)
pvmove "$dev1" "$dev5"
lv_is_on_pvs $vg ${lv1}_foo "$dev5"
lv_is_on_pvs $vg $lv1 "$dev5"
check_dev_sum_ $(lvdev_ $vg $lv1)
pvmove -n $lv1 "$dev5" "$dev4"
lv_is_on_pvs $vg $lv1 "$dev4"
lv_is_on_pvs $vg ${lv1}_foo "$dev5"
check_dev_sum_ $(lvdev_ $vg $lv1)
lvremove -ff $vg

printf "##\n# Testing pvmove of stripe LV\n##\n"
lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -l 4 -i 2 -n $lv1 $vg "$dev1" "$dev2"
lv_is_on_pvs $vg ${lv1}_foo "$dev1"
lv_is_on_pvs $vg $lv1 "$dev1" "$dev2"
save_dev_sum_ $(lvdev_ $vg $lv1)
pvmove "$dev1" "$dev5"
lv_is_on_pvs $vg ${lv1}_foo "$dev5"
lv_is_on_pvs $vg $lv1 "$dev2" "$dev5"
check_dev_sum_ $(lvdev_ $vg $lv1)
pvmove -n $lv1 "$dev5" "$dev4"
lv_is_on_pvs $vg $lv1 "$dev2" "$dev4"
lv_is_on_pvs $vg ${lv1}_foo "$dev5"
check_dev_sum_ $(lvdev_ $vg $lv1)
lvremove -ff $vg

printf "##\n# Testing pvmove of mirror LV\n##\n"
lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -l 2 --type mirror -m 1 -n $lv1 $vg "$dev1" "$dev2"
lv_is_on_pvs $vg ${lv1}_foo "$dev1"
lv_is_on_pvs $vg $lv1 "$dev1" "$dev2"
save_dev_sum_ $(lvdev_ $vg $lv1)
pvmove "$dev1" "$dev5"
lv_is_on_pvs $vg ${lv1}_foo "$dev5"
lv_is_on_pvs $vg $lv1 "$dev2" "$dev5"
check_dev_sum_ $(lvdev_ $vg $lv1)
pvmove -n $lv1 "$dev5" "$dev4"
lv_is_on_pvs $vg $lv1 "$dev2" "$dev4"
lv_is_on_pvs $vg ${lv1}_foo "$dev5"
check_dev_sum_ $(lvdev_ $vg $lv1)
lvremove -ff $vg

# Dummy LV and snap share dev1, while origin is on dev2
printf "##\n# Testing pvmove of snapshot LV\n##\n"
lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
lvcreate -l 2 -n $lv1 $vg "$dev2"
lvcreate -s $vg/$lv1 -l 2 -n snap "$dev1"
lv_is_on_pvs $vg ${lv1}_foo "$dev1"
lv_is_on_pvs $vg snap "$dev1"
save_dev_sum_ $(lvdev_ $vg snap)
pvmove "$dev1" "$dev5"
lv_is_on_pvs $vg ${lv1}_foo "$dev5"
lv_is_on_pvs $vg snap "$dev5"
check_dev_sum_ $(lvdev_ $vg snap)
pvmove -n snap "$dev5" "$dev4"
lv_is_on_pvs $vg snap "$dev4"
lv_is_on_pvs $vg ${lv1}_foo "$dev5"
check_dev_sum_ $(lvdev_ $vg snap)
lvremove -ff $vg

##
# RAID
##
if aux target_at_least dm-raid 1 3 5; then
	printf "##\n# Testing pvmove of RAID1 LV\n##\n"
	lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
	lvcreate -l 2 --type raid1 -m 1 -n $lv1 $vg "$dev1" "$dev2"
	lv_is_on_pvs $vg ${lv1}_foo "$dev1"
	lv_is_on_pvs $vg $lv1 "$dev1" "$dev2"
	save_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove "$dev1" "$dev5"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	lv_is_on_pvs $vg $lv1 "$dev2" "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove -n $lv1 "$dev5" "$dev4"
	lv_is_on_pvs $vg $lv1 "$dev2" "$dev4"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	lvremove -ff $vg

	printf "##\n# Testing pvmove of RAID10 LV\n##\n"
	lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
	lvcreate -l 4 --type raid10 -i 2 -m 1 -n $lv1 $vg \
			"$dev1" "$dev2" "$dev3" "$dev4"
	lv_is_on_pvs $vg ${lv1}_foo "$dev1"
	lv_is_on_pvs $vg $lv1 "$dev1" "$dev2" "$dev3" "$dev4"
	save_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove "$dev1" "$dev5"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	lv_is_on_pvs $vg $lv1 "$dev2" "$dev3" "$dev4" "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove -n $lv1 "$dev5" "$dev1"
	lv_is_on_pvs $vg $lv1 "$dev1" "$dev2" "$dev3" "$dev4"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	lvremove -ff $vg

	printf "##\n# Testing pvmove of RAID5 LV\n##\n"
	lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
	lvcreate -l 4 --type raid5 -i 2 -n $lv1 $vg \
			"$dev1" "$dev2" "$dev3"
	lv_is_on_pvs $vg ${lv1}_foo "$dev1"
	lv_is_on_pvs $vg $lv1 "$dev1" "$dev2" "$dev3"
	save_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove "$dev1" "$dev5"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	lv_is_on_pvs $vg $lv1 "$dev2" "$dev3" "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove -n $lv1 "$dev5" "$dev4"
	lv_is_on_pvs $vg $lv1 "$dev2" "$dev3" "$dev4"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	lvremove -ff $vg
fi

##
# Thin
##
if aux target_at_least dm-thin-pool 1 8 0; then

	printf "##\n# Testing pvmove of thin-pool LV\n##\n"
	lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
	lvcreate -T $vg/$lv1 -l 4 "$dev1"
	lv_is_on_pvs $vg ${lv1}_foo "$dev1"
	lv_is_on_pvs $vg $lv1 "$dev1"
	save_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove "$dev1" "$dev5"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	lv_is_on_pvs $vg $lv1 "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove -n $lv1 "$dev5" "$dev4"
	lv_is_on_pvs $vg $lv1 "$dev4"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	lvremove -ff $vg

	printf "##\n# Testing pvmove of thin LV\n##\n"
	lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
	lvcreate -T $vg/${lv1}_pool -l 4 -V 8 -n $lv1 "$dev1"
	lv_is_on_pvs $vg ${lv1}_foo "$dev1"
	lv_is_on_pvs $vg $lv1 "$dev1"
	save_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove "$dev1" "$dev5"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	lv_is_on_pvs $vg $lv1 "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove -n $lv1 "$dev5" "$dev4"
	lv_is_on_pvs $vg $lv1 "$dev4"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	lvremove -ff $vg

	printf "##\n# Testing pvmove of thin LV on RAID\n##\n"
	lvcreate -l 2 -n ${lv1}_foo $vg "$dev1"
	lvcreate --type raid1 -m 1 -l 4 -n ${lv1}_raid1_pool $vg "$dev1" "$dev2"
	lvcreate --type raid1 -m 1 -l 4 -n ${lv1}_raid1_meta $vg "$dev1" "$dev2"
	lvconvert --thinpool $vg/${lv1}_raid1_pool \
		--poolmetadata ${lv1}_raid1_meta
	lvcreate -T $vg/${lv1}_raid1_pool -V 8 -n $lv1
	lv_is_on_pvs $vg ${lv1}_foo "$dev1"
	lv_is_on_pvs $vg $lv1 "$dev1" "$dev2"
	save_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove "$dev1" "$dev5"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	lv_is_on_pvs $vg $lv1 "$dev2" "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	pvmove -n $lv1 "$dev5" "$dev4"
	lv_is_on_pvs $vg $lv1 "$dev2" "$dev4"
	lv_is_on_pvs $vg ${lv1}_foo "$dev5"
	check_dev_sum_ $(lvdev_ $vg $lv1)
	lvremove -ff $vg
fi
