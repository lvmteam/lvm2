#!/bin/sh
# Copyright (C) 2011-2012 Red Hat, Inc. All rights reserved.
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


# trims only leading prefix, we should not need trim trailing spaces
trim_() {
	#local var=${var%"${var##*[! ]}"}  # remove trailing space characters
	echo ${1#"${1%%[! ]*}"} # remove leading space characters
}

pv_field() {
	trim_ "$(pvs --noheadings -o $2 ${@:3} $1)"
}

vg_field() {
	trim_ "$(vgs --noheadings -o $2 ${@:3} $1)"
}

lv_field() {
	trim_ "$(lvs --noheadings -o $2 ${@:3} $1)"
}

lv_devices() {
	lv_field $1 devices -a "${@:2}" | sed 's/([^)]*)//g; s/,/ /g'
}

unset LVM_VALGRIND
"$@"
