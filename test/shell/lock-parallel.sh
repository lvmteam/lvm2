#!/bin/sh
# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Test parallel use of lvm commands and check locks aren't dropped
# RHBZ: https://bugzilla.redhat.com/show_bug.cgi?id=1049296

. lib/inittest

which mkfs.ext3 || skip

aux prepare_vg

lvcreate -L10 -n $lv1 $vg
lvcreate -l1 -n $lv2 $vg
mkfs.ext3 "$DM_DEV_DIR/$vg/$lv1"

# Slowdown PV for resized LV
aux delay_dev "$dev1" 20 20

lvresize -L-5 -r $vg/$lv1 &

# Let's wait till resize starts
sleep 2

lvremove -f $vg/$lv2

wait

aux enable_dev "$dev1"

# Check removed $lv2 does not reappear
not check lv_exists $vg $lv2

vgremove -ff $vg
