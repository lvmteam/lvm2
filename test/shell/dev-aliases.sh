#!/usr/bin/env bash

# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 10 0 || skip

aux prepare_vg 3

#
# This lvconvert command will deactivate LV1, then internally create a new
# lv, lvol0, as a poolmetadataspare, then activate lvol0 to zero it.
# lvol0 will get the same major:minor that LV1 had.  When the code gets
# the struct dev for lvol0, the new path to lvol0 is added to the
# dev-cache with it's major:minor.  That major:minor already exists in
# dev-cache and has the stale LV1 as an alias.  So the path to lvol0 is
# added as an alias to the existing struct dev (with the correct
# major:minor), but that struct dev has the stale LV1 path on its aliases
# list.  The code will now validate all the aliases before returning the
# dev for lvol0, and will find that the LV1 path is stale and remove it
# from the aliases.  That will prevent the stale path from being used for
# the dev in place of the new path.
#
# The preferred_name is set to /dev/mapper so that if the stale path still
# exists, that stale path would be used as the name for the dev, and the
# wiping code would fail to open that stale name.
#

lvcreate -n $lv1 -L32M $vg "$dev1"
lvcreate -n $lv2 -L16M $vg "$dev2"
lvconvert -y --type cache-pool --poolmetadata $lv2 --cachemode writeback $vg/$lv1 --config='devices { preferred_names=["/dev/mapper/"] }' 
lvremove -y $vg/$lv1

lvcreate -n $lv1 -L32M $vg "$dev1"
lvcreate -n $lv2 -L16M $vg "$dev2"
lvconvert -y --type cache-pool --poolmetadata $lv2 $vg/$lv1
lvremove -y $vg/$lv1

# TODO: add more validation of dev aliases being specified as command
# args in combination with various preferred_names settings.

vgremove -ff  $vg
