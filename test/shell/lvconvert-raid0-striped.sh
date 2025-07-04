#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

aux have_raid 1 7 0 || skip

aux prepare_vg 3 16

lvcreate -aey --type raid0 -i 3 -l3 -n $lv $vg
lvconvert -y --type striped $vg/$lv
check lv_field $vg/$lv segtype "striped"
vgremove -ff $vg
