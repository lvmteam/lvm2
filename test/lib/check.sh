#!/usr/bin/env bash
# Copyright (C) 2010-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# check.sh: assert various things about volumes

# USAGE:
#  check linear VG LV
#  check lv_on VG LV PV

#  check mirror VG LV [LOGDEV|core]
#  check mirror_nonredundant VG LV
#  check mirror_legs VG LV N
#  check mirror_images_on VG LV DEV [DEV...]

# ...

test -z "$BASH" || set -e -o pipefail

die() {
	rm -f debug.log
	echo -e "$@" >&2
	return 1
}

lvl() {
	lvs -a --noheadings "$@"
}

lvdevices() {
	get lv_devices "$@"
}

mirror_images_redundant() {
	local vg=$1
	local lv=$vg/$2
	lvs -a $vg -o+devices
	for i in $(lvdevices $lv); do
		echo "# $i:"
		lvdevices $vg/$i | sort | uniq
	done > check.tmp.all

	(grep -v ^# check.tmp.all || true) | sort | uniq -d > check.tmp

	test $(cat check.tmp | wc -l) -eq 0 || \
		die "mirror images of $lv expected redundant, but are not:" \
			$(cat check.tmp.all)
}

lv_err_list_() {
	(echo "$2" | not grep -m 1 -q "$1") || \
		echo "$3 on [ $(echo "$2" | grep "$1" | cut -b3- | tr '\n' ' ')] "
}

lv_on_diff_() {
	declare -a devs=("${!1}") # pass in shell array
	local expect=( "${@:4}" ) # make an array starting from 4th args...
	local diff_e

	# Find diff between 2 shell arrays, print them as stdin files
	diff_e=$(diff <(printf "%s\n" "${expect[@]}" | sort | uniq ) <(printf "%s\n" "${devs[@]}") ) ||
		die "LV $2/$3 $(lv_err_list_ "^>" "${diff_e}" found)$(lv_err_list_ "^<" "${diff_e}" "not found")."
}

# list devices for given LV
lv_on() {
	local devs

	devs=( $(lvdevices "$1/$2" | sort | uniq ) )

	lv_on_diff_ devs[@] "${@}"
}

# list devices for given LV and all its subdevices
lv_tree_on() {
	local devs

	# Get sorted list of devices
	devs=( $(get lv_tree_devices "$1" "$2") )

	lv_on_diff_ devs[@] "${@}"
}

mirror_images_on() {
	local vg=$1
	local lv=$2
	shift 2
	for i in $(lvdevices $lv); do
		lv_on $vg $lv $1
		shift
	done
}

mirror_log_on() {
	local vg=$1
	local lv=$2
	local where=$3
	if test "$where" = "core"; then
		get lv_field $vg/$lv mirror_log | not grep mlog
	else
		lv_on $vg ${lv}_mlog "$where"
	fi
}

lv_is_contiguous() {
	local lv=$1/$2
	test $(lvl --segments $lv | wc -l) -eq 1 || \
		die "LV $lv expected to be contiguous, but is not:" \
			$(lvl --segments $lv)
}

lv_is_clung() {
	local lv=$1/$2
	test $(lvdevices $lv | sort | uniq | wc -l) -eq 1 || \
		die "LV $lv expected to be clung, but is not:" \
			$(lvdevices $lv | sort | uniq)
}

mirror_images_contiguous() {
	for i in $(lvdevices $1/$2); do
		lv_is_contiguous $1 $i
	done
}

mirror_images_clung() {
	for i in $(lvdevices $1/$2); do
		lv_is_clung $1 $i
	done
}

mirror() {
	mirror_nonredundant "$@"
	mirror_images_redundant $1 $2
}

mirror_nonredundant() {
	local lv=$1/$2
	local attr=$(get lv_field $lv attr)
	(echo "$attr" | grep "^......m...$" >/dev/null) || {
		if (echo "$attr" | grep "^o.........$" >/dev/null) &&
		   lvs -a | fgrep "[${2}_mimage" >/dev/null; then
			echo "TEST WARNING: $lv is a snapshot origin and looks like a mirror,"
			echo "assuming it is actually a mirror"
		else
			die "$lv expected a mirror, but is not:" \
				$(lvs $lv)
		fi
	}
	test -z "$3" || mirror_log_on $1 $2 "$3"
}

mirror_legs() {
	local expect=$3
	test "$expect" -eq $(lvdevices $1/$2 | wc -w)
}

mirror_no_temporaries() {
	local vg=$1
	local lv=$2
	(lvl -o name $vg | grep $lv | not grep "tmp") || \
		die "$lv has temporary mirror images unexpectedly:" \
			$(lvl $vg | grep $lv)
}

linear() {
	local lv=$1/$2
	test $(get lv_field $lv stripes -a) -eq 1 || \
		die "$lv expected linear, but is not:" \
			$(lvl $lv -o+devices)
}

# in_sync <VG> <LV>
# Works for "mirror" and "raid*"
in_sync() {
	local a
	local b
	local idx
	local type
	local snap=""
	local lvm_name="$1/$2"
	local dm_name=$(echo $lvm_name | sed s:-:--: | sed s:/:-:)

	if ! a=(`dmsetup status $dm_name`); then
		die "Unable to get sync status of $1"
	elif [ ${a[2]} = "snapshot-origin" ]; then
		if ! a=(`dmsetup status ${dm_name}-real`); then
			die "Unable to get sync status of $1"
		fi
		snap=": under snapshot"
	fi

	if [ ${a[2]} = "raid" ]; then
		# 6th argument is the sync ratio for RAID
		idx=6
		type=${a[3]}
	elif [ ${a[2]} = "mirror" ]; then
		# 4th Arg tells us how far to the sync ratio
		idx=$((${a[3]} + 4))
		type=${a[2]}
	else
		die "Unable to get sync ratio for target type '${a[2]}'"
	fi

	b=( $(echo ${a[$idx]} | sed s:/:' ':) )

	if [ ${b[0]} != ${b[1]} ]; then
		echo "$lvm_name ($type$snap) is not in-sync"
		return 1
	fi

	if [[ ${a[$(($idx - 1))]} =~ a ]]; then
		die "$lvm_name ($type$snap) in-sync, but 'a' characters in health status"
	fi

	echo "$lvm_name ($type$snap) is in-sync"
	return 0
}

active() {
	local lv=$1/$2
	(get lv_field $lv attr | grep "^....a.....$" >/dev/null) || \
		die "$lv expected active, but lvs says it's not:" \
			$(lvl $lv -o+devices)
	dmsetup info $1-$2 >/dev/null ||
		die "$lv expected active, lvs thinks it is but there are no mappings!"
}

inactive() {
	local lv=$1/$2
	(get lv_field $lv attr | grep "^....[-isd].....$" >/dev/null) || \
		die "$lv expected inactive, but lvs says it's not:" \
			$(lvl $lv -o+devices)
	not dmsetup info $1-$2 2>/dev/null || \
		die "$lv expected inactive, lvs thinks it is but there are mappings!" 
}

# Check for list of LVs from given VG
lv_exists() {
	local vg=$1
	local lv=
	while [ $# -gt 1 ]; do
		shift
		lv="$lv $vg/$1"
	done
	lvl $lv &>/dev/null || \
		die "$lv expected to exist but does not"
}

pv_field() {
	local actual=$(get pv_field "$1" "$2" "${@:4}")
	test "$actual" = "$3" || \
		die "pv_field: PV=\"$1\", field=\"$2\", actual=\"$actual\", expected=\"$3\""
}

vg_field() {
	local actual=$(get vg_field "$1" "$2" "${@:4}")
	test "$actual" = "$3" || \
		die "vg_field: vg=$1, field=\"$2\", actual=\"$actual\", expected=\"$3\""
}

lv_field() {
	local actual=$(get lv_field "$1" "$2" "${@:4}")
	test "$actual" = "$3" || \
		die "lv_field: lv=$lv, field=\"$2\", actual=\"$actual\", expected=\"$3\""
}

compare_fields() {
	local cmd1=$1
	local obj1=$2
	local field1=$3
	local cmd2=$4
	local obj2=$5
	local field2=$6
	local val1=$($cmd1 --noheadings -o "$field1" "$obj1")
	local val2=$($cmd2 --noheadings -o "$field2" "$obj2")
	test "$val1" = "$val2" || \
		die "compare_fields $obj1($field1): $val1 $obj2($field2): $val2"
}

compare_vg_field() {
	local vg1=$1
	local vg2=$2
	local field=$3
	local val1=$(vgs --noheadings -o "$field" $vg1)
	local val2=$(vgs --noheadings -o "$field" $vg2)
	test "$val1" = "$val2" || \
		die "compare_vg_field: $vg1: $val1, $vg2: $val2"
}

pvlv_counts() {
	local local_vg=$1
	local num_pvs=$2
	local num_lvs=$3
	local num_snaps=$4
	lvs -o+devices $local_vg
	vg_field $local_vg pv_count $num_pvs
	vg_field $local_vg lv_count $num_lvs
	vg_field $local_vg snap_count $num_snaps
}

# Compare md5 check generated from get dev_md5sum
dev_md5sum() {
	md5sum -c "md5.$1-$2" || \
		(get lv_field $1/$2 "name,size,seg_pe_ranges"
		 die "LV $1/$2 has different MD5 check sum!")
}

#set -x
unset LVM_VALGRIND
"$@"
