#!/bin/sh
# Copyright (C) 2011-2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# 'Exercise duplicate metadata diagnostics'

. lib/inittest

test -e LOCAL_LVMPOLLD && skip

aux prepare_devs 3

pvcreate "$dev1"

dd if="$dev1" of=backup_dev1 bs=256K count=1

vgcreate --metadatasize 128k $vg1 "$dev1"

# copy mda
dd if="$dev1" of="$dev2" bs=256K count=1
dd if="$dev1" of="$dev3" bs=256K count=1

pvs "$dev3" -o pv_uuid

vgs $vg1

dd if=backup_dev1 of="$dev3" bs=256K count=1
pvs
#-vvvv
# TODO: Surely needs more inspecition about correct
#       behavior for such case
# vgs $vg1
