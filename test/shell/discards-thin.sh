#!/bin/sh

# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
# test support of thin discards
#

. lib/test

#
# Main
#
aux have_thin 1 1 0 || skip

aux prepare_pvs 2 64

vgcreate $vg -s 64K $(cat DEVICES)

# Create named pool only
lvcreate -l1 --discards ignore -T $vg/pool
check lv_field $vg/pool discards "ignore"
lvcreate -l1 --discards nopassdown -T $vg/pool1
check lv_field $vg/pool1 discards "nopassdown"
lvcreate -l1 --discards passdown -T $vg/pool2
check lv_field $vg/pool2 discards "passdown"

lvchange --discards nopassdown $vg/pool2

# cannot convert active  ignore -> passdown
not lvchange --discards passdown $vg/pool

# cannot convert active  nopassdown -> ignore
not lvchange --discards ignore $vg/pool1

# deactivate
lvchange -an $vg/pool $vg/pool1
lvchange --discards passdown $vg/pool
check lv_field $vg/pool discards "passdown"
lvchange --discards ignore $vg/pool1
check lv_field $vg/pool1 discards "ignore"

vgremove -ff $vg
