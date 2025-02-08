#!/usr/bin/env bash

# Copyright (C) 2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Check pvmove behaviour when its device are kept open

SKIP_WITH_LVMLOCKD=1

. lib/inittest

_create_lv()
{
	lvremove -f $vg
	lvcreate -Zn -l20 -n $lv1 $vg "$dev1"
}

_keep_open()
{
	# wait till device we want to keep open appears
	while test ! -e "$DM_DEV_DIR/mapper/$vg-$1" ; do sleep .1 ; done
	# keep device open for 3 seconds
	sleep ${2-3} < "$DM_DEV_DIR/mapper/$vg-$1"
}

aux target_at_least dm-mirror 1 2 0 || skip

aux prepare_vg 3

aux delay_dev "$dev3" 0 1 "$(get first_extent_sector "$dev3"):"

# do not waste 'testing' time on 'retry deactivation' loops
aux lvmconf 'activation/retry_deactivation = 0' \
	    'activation/raid_region_size = 16'

# fallback to mirror throttling
# this does not work too well with fast CPUs
test -f HAVE_DM_DELAY || { aux throttle_dm_mirror || skip ; }

########################################################
# pvmove operation finishes, while 1 mirror leg is open
########################################################

_create_lv
_keep_open pvmove0_mimage_0 &

# pvmove fails in such case
not pvmove --atomic "$dev1" "$dev3" >out 2>&1

wait

cat out
grep "ABORTING: Failed" out
lvs -ao+seg_pe_ranges $vg
# but LVs were already moved
check lv_on $vg $lv1 "$dev3"
lvs -a $vg

# orphan LV should be visible with error segment and removable
check lv_field $vg/pvmove0_mimage_0 layout "error"
check lv_field $vg/pvmove0_mimage_0 role "public"
lvremove -f $vg/pvmove0_mimage_0


##########################################
# abort pvmove while 1 mirror leg is open
##########################################

_create_lv
_keep_open pvmove0_mimage_1 &

# with background mode - it's forking polling
pvmove -b --atomic "$dev1" "$dev3" >out 2>&1
aux wait_pvmove_lv_ready "$vg-pvmove0"

pvmove --abort >out1 2>&1

wait

cat out
cat out1
grep "ABORTING: Failed" out1
lvs -ao+seg_pe_ranges $vg
# hopefully we managed to abort before pvmove finished
check lv_on $vg $lv1 "$dev1"
check lv_field $vg/pvmove0_mimage_1 layout "error"
check lv_field $vg/pvmove0_mimage_1 role "public"
lvremove -f $vg/pvmove0_mimage_1


#############################################
# keep pvmove0 open while it tries to finish
#############################################

_create_lv
_keep_open pvmove0 &

not pvmove --atomic "$dev1" "$dev3" >out 2>&1

wait

cat out
grep "ABORTING: Unable to deactivate" out

check lv_field $vg/pvmove0 layout "error"
check lv_field $vg/pvmove0 role "public"
lvremove -f $vg/pvmove0


################################################
# keep all pvmove volumes open
################################################

_create_lv
_keep_open pvmove0_mimage_0 &
_keep_open pvmove0_mimage_1 &
_keep_open pvmove0 &

not pvmove --atomic "$dev1" "$dev3" >out 2>&1

wait

cat out
grep "ABORTING: Unable to deactivate" out

lvremove -f $vg/pvmove0_mimage_0
lvremove -f $vg/pvmove0_mimage_1
lvremove -f $vg/pvmove0


# Restore throttling
aux restore_dm_mirror

vgremove -ff $vg
