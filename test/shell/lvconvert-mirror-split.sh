#!/usr/bin/env bash

# Copyright (C) 2018 - 2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Check --splitmirrors for mirror segtype

. lib/inittest

aux prepare_vg 3

###########################################
# Mirror split tests
###########################################
# 3-way to 2-way/linear
lvcreate -aey --type mirror -m 2 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
lvconvert --splitmirrors 1 -n $lv2 -v $vg/$lv1

check lv_exists $vg $lv1
check linear $vg $lv2
check active $vg $lv2

lvremove -f $vg

#################################################
# Mirror split when mirror devices are held open
#################################################

# do not waste 'testing' time on 'retry deactivation' loops
aux lvmconf 'activation/retry_deactivation = 0'

lvcreate -aey --type mirror -m 1 -l 2 -n $lv1 $vg
aux wait_for_sync $vg $lv1
sleep 2 < "$DM_DEV_DIR/mapper/${vg}-${lv1}_mimage_0" &
sleep 2 < "$DM_DEV_DIR/mapper/${vg}-${lv1}_mlog" &

not lvconvert --splitmirrors 1 -n $lv2 -v $vg/$lv1

wait

check lv_field $vg/${lv1}_mimage_0 layout "error"
check lv_field $vg/${lv1}_mlog layout "error"
check linear $vg $lv2

lvremove -f $vg/${lv1}_mimage_0
lvremove -f $vg/${lv1}_mlog

# FIXME: ensure no residual devices

vgremove -ff $vg
