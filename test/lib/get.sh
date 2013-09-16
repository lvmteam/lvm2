#!/bin/sh
# Copyright (C) 2011-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# get.sh: get various values from volumes
#
# USAGE:
#  get pv_field PV field [pvs params]
#  get vg_field VG field [vgs params]
#  get lv_field LV field [lvs params]
#
#  get lv_devices LV     [lvs params]

test -z "$BASH" || set -e -o pipefail

# trims only leading prefix and suffix
trim_() {
	rm -f debug.log             # drop log, command was ok
	local var=${1%${1##*[! ]}}  # remove trailing space characters
	echo "${var#${var%%[! ]*}}" # remove leading space characters
}

pv_field() {
	trim_ "$(pvs --config 'log{prefix=""}' --noheadings -o $2 ${@:3} $1)"
}

vg_field() {
	trim_ "$(vgs --config 'log{prefix=""}' --noheadings -o $2 ${@:3} $1)"
}

lv_field() {
	trim_ "$(lvs --config 'log{prefix=""}' --noheadings -o $2 ${@:3} $1)"
}

lv_devices() {
	lv_field "$1" devices -a "${@:2}" | sed 's/([^)]*)//g; s/,/\n/g'
}

lv_field_lv_() {
	lv_field "$1" "$2" -a --unbuffered | sed 's/\[//; s/]//'
}

lv_tree_devices_() {
	local lv="$1/$2"
	local type=$(lv_field "$lv" segtype -a --unbuffered | head -n 1)
	local orig=$(lv_field_lv_ "$lv" origin)
	# FIXME: should we count in also origins ?
	#test -z "$orig" || lv_tree_devices_ $1 $orig
	case "$type" in
	linear|striped)
		lv_devices "$lv"
		;;
	mirror|raid*)
		local log=$(lv_field_lv_ "$lv" mirror_log)
		test -z "$log" || lv_tree_devices_ "$1" "$log"
		for i in $(lv_devices "$lv")
			do lv_tree_devices_ "$1" "$i"; done
		;;
	thin)
		lv_tree_devices_ "$1" "$(lv_field_lv_ $lv pool_lv)"
		;;
	thin-pool)
		lv_tree_devices_ "$1" "$(lv_field_lv_ $lv data_lv)"
		lv_tree_devices_ "$1" "$(lv_field_lv_ $lv metadata_lv)"
		;;
	esac
}

lv_tree_devices() {
	lv_tree_devices_ "$@" | sort | uniq
}

#set -x
unset LVM_VALGRIND
"$@"
