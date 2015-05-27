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
	declare -a xdevs=("${!1}") # pass in shell array
	local expect=( "${@:4}" ) # make an array starting from 4th args...
	local diff_e

	# Find diff between 2 shell arrays, print them as stdin files
	printf "%s\n" "${expect[@]}" | sort | uniq >_lv_on_diff1
	printf "%s\n" "${xdevs[@]}" >_lv_on_diff2
	diff_e=$(diff _lv_on_diff1 _lv_on_diff2) ||
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
		if [ ${a[$(($idx + 1))]} != "idle" ]; then
			echo "$lvm_name ($type$snap) is not in-sync"
			return 1
		fi
	elif [ ${a[2]} = "mirror" ]; then
		# 4th Arg tells us how far to the sync ratio
		idx=$((${a[3]} + 4))
		type=${a[2]}
	else
		die "Unable to get sync ratio for target type '${a[2]}'"
	fi

	b=( $(echo ${a[$idx]} | sed s:/:' ':) )

	if [ ${b[0]} -eq 0 -o ${b[0]} != ${b[1]} ]; then
		echo "$lvm_name ($type$snap) is not in-sync"
		return 1
	fi

	if [[ ${a[$(($idx - 1))]} =~ a ]]; then
		die "$lvm_name ($type$snap) in-sync, but 'a' characters in health status"
	fi

	echo "$lvm_name ($type$snap) is in-sync"
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
	test -n "$lv" || lv=$vg
	lvl $lv &>/dev/null || \
		die "$lv expected to exist but does not"
}

lv_not_exists() {
	local vg=$1
	if test $# -le 1 ; then
		if lvl $vg &>/dev/null ; then
			die "$vg expected to not exist but it does!"
		fi
	else
		while [ $# -gt 1 ]; do
			shift
			lvl $vg/$1 &>/dev/null || continue
			die "$vg/$1 expected to not exist but it does!"
		done
	fi
	rm -f debug.log
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

vg_attr_bit() {
	local actual=$(get vg_field "$2" vg_attr "${@:4}")
	local offset=$1
	case "$offset" in
	  perm*) offset=0 ;;
	  resiz*) offset=1 ;;
	  export*) offset=2 ;;
	  partial) offset=3 ;;
	  alloc*) offset=4 ;;
	  cluster*) offset=5 ;;
	esac
	test "${actual:$offset:1}" = "$3" || \
		die "vg_attr_bit: vg=$2, ${offset} bit of \"$actual\" is \"${actual:$offset:1}\", but expected \"$3\""
}

lv_field() {
	local actual=$(get lv_field "$1" "$2" "${@:4}")
	test "$actual" = "$3" || \
		die "lv_field: lv=$1, field=\"$2\", actual=\"$actual\", expected=\"$3\""
}

lv_attr_bit() {
	local actual=$(get lv_field "$2" lv_attr "${@:4}")
	local offset=$1
	case "$offset" in
	  type) offset=0 ;;
	  perm*) offset=1 ;;
	  alloc*) offset=2 ;;
	  fixed*) offset=3 ;;
	  state) offset=4 ;;
	  open) offset=5 ;;
	  target) offset=6 ;;
	  zero) offset=7 ;;
	  health) offset=8 ;;
	  skip) offset=9 ;;
	esac
	test "${actual:$offset:1}" = "$3" || \
		die "lv_attr_bit: lv=$2, ${offset} bit of \"$actual\" is \"${actual:$offset:1}\", but expected \"$3\""
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

sysfs() {
	# read maj min and also convert hex to decimal
	local maj=$(($(stat -L --printf=0x%t "$1")))
	local min=$(($(stat -L --printf=0x%T "$1")))
	local P="/sys/dev/block/$maj:$min/$2"
	local val=$(< "$P") || return 0 # no sysfs ?
	test "$val" -eq "$3" || \
		die "$1: $P = $val differs from expected value $3!"
}

#set -x
unset LVM_VALGRIND
"$@"
