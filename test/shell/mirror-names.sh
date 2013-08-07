#!/bin/sh
# Copyright (C) 2007-2012 Red Hat, Inc. All rights reserved.
# Copyright (C) 2007-2008 NEC Corporation
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

test_description="check namings of mirrored LV"

. lib/test

# ---------------------------------------------------------------------
# Utilities

lv_devices_() {
	local d
	local lv=$1
	shift
	local devices=$@
	local devs=$(get lv_field $lv devices -a | sed 's/([0-9]*)//g; s/ //g; s/,/ /g')

	for d in $devs; do
		(echo $devices | grep $d) || return 1
		devices=$(echo $devices | sed "s/$d//")
	done

	test -z "$(echo $devices | sed 's/ //g')"
}

lv_mirror_log_() {
	test $(get lv_field $1 mirror_log) = $2
}

lv_convert_lv_() {
	get lv_field $1 convert_lv
}

# ---------------------------------------------------------------------
# Common environment setup/cleanup for each sub testcases

check_and_cleanup_lvs_() {
	lvs -a -o+devices $vg
	lvremove -ff $vg
	(dm_table | not grep $vg) || \
		die "ERROR: lvremove did leave some some mappings in DM behind!"
}

# ---------------------------------------------------------------------
# Initialize PVs and VGs

aux prepare_vg 5 80

check_and_cleanup_lvs_

# ---------------------------------------------------------------------
# basic

#COMM "init: lvcreate"

#COMM "mirror images are ${lv1}_mimage_x"
lvcreate -an -Zn -l2 --type mirror -m1 -n $lv1 $vg
lv_devices_ $vg/$lv1 ${lv1}_mimage_0 ${lv1}_mimage_1

#COMM "mirror log is ${lv1}_mlog"
lv_mirror_log_ $vg/$lv1 ${lv1}_mlog

# "cleanup"
check_and_cleanup_lvs_

#COMM "mirror with name longer than 22 characters (bz221322)"
name="LVwithanamelogerthan22characters_butidontwonttocounthem"
lvcreate -an -Zn --type mirror -m1 -l2 -n $name $vg
lvs $vg/$name
check_and_cleanup_lvs_

# ---------------------------------------------------------------------
# lvrename

#COMM "init: lvrename"

#COMM "renamed mirror names: $lv1 to $lv2"
lvcreate -an -Zn -l2 --type mirror -m1 -n $lv1 $vg
lvrename $vg/$lv1 $vg/$lv2
lv_devices_ $vg/$lv2 ${lv2}_mimage_0 ${lv2}_mimage_1
lv_mirror_log_ $vg/$lv2 ${lv2}_mlog

#COMM "cleanup"
check_and_cleanup_lvs_

# ---------------------------------------------------------------------
# lvconvert

#COMM "init: lvconvert"

#COMM "converting mirror names is ${lv1}_mimagetmp_2"
lvcreate -aey -l2 --type mirror -m1 -n $lv1 $vg
lvconvert -m+1 -i+40 -b $vg/$lv1
convlv=$(lv_convert_lv_ $vg/$lv1)
test $convlv = ${lv1}_mimagetmp_2
lv_devices_ $vg/$lv1 $convlv ${lv1}_mimage_2
lv_devices_ $vg/$convlv ${lv1}_mimage_0 ${lv1}_mimage_1
lv_mirror_log_ $vg/$convlv ${lv1}_mlog

#COMM "mirror log name after re-adding is ${lv1}_mlog" \
lvconvert -f --mirrorlog core $vg/$lv1
lvconvert --mirrorlog disk $vg/$lv1
convlv=$(lv_convert_lv_ $vg/$lv1)
lv_devices_ $vg/$lv1 $convlv ${lv1}_mimage_2
lv_devices_ $vg/$convlv ${lv1}_mimage_0 ${lv1}_mimage_1
lv_mirror_log_ $vg/$convlv ${lv1}_mlog

#COMM "renamed converting mirror names: $lv1 to $lv2" \
lvrename $vg/$lv1 $vg/$lv2
convlv=$(lv_convert_lv_ $vg/$lv2)
lv_devices_ $vg/$lv2 $convlv ${lv2}_mimage_2
lv_devices_ $vg/$convlv ${lv2}_mimage_0 ${lv2}_mimage_1
lv_mirror_log_ $vg/$convlv ${lv2}_mlog

#COMM "cleanup"
check_and_cleanup_lvs_

# Temporary mirror log should have "_mlogtmp_<n>" suffix
# but currently lvconvert doesn't have an option to add the log.
# If such feature is added in future, a test for that should
# be added.

# ---------------------------------------------------------------------
