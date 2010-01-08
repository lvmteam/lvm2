#!/bin/bash
# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. ./test-utils.sh

prepare_vg 4

cleanup() {
	vgreduce --removemissing $vg
	for d in "$@"; do enable_dev $d; done
	for d in "$@"; do vgextend $vg $d; done
	lvremove -ff $vg/mirror
	lvcreate -m 1 -L 1 -n mirror $vg
}

repair() {
	lvconvert -i 1 --repair --use-policies --config "$1" $vg/mirror
}

lvcreate -m 1 -L 1 -n mirror $vg
lvchange -a n $vg/mirror

disable_dev $dev1
lvchange --partial -a y $vg/mirror
repair 'activation { mirror_image_fault_policy = "remove" }'
lvs | grep -- -wi-a- # non-mirror
cleanup $dev1

disable_dev $dev1
repair 'activation { mirror_image_fault_policy = "replace" }'
lvs | grep -- mwi-a- # mirror
lvs | grep mirror_mlog
cleanup $dev1

disable_dev $dev1
repair 'activation { mirror_device_fault_policy = "replace" }'
lvs | grep -- mwi-a- # mirror
lvs | grep mirror_mlog
cleanup $dev1

disable_dev $dev2 $dev4
# no room for repair, downconversion should happen
repair 'activation { mirror_image_fault_policy = "replace" }'
lvs | grep -- -wi-a-
cleanup $dev2 $dev4

disable_dev $dev2 $dev4
# no room for new log, corelog conversion should happen
repair 'activation { mirror_image_fault_policy = "replace" }'
lvs
lvs | grep -- mwi-a-
lvs | not grep mirror_mlog
cleanup $dev2 $dev4
